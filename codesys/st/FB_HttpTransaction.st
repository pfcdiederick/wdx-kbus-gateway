// ===========================================================================
// FB_HttpTransaction  -  ONE HTTP request (byte based, fast completion)
// Object type in CODESYS: POU, Function Block, language ST
// Library: CAA Net Base Services (namespace NBS) + Standard
//
// Flow: connect -> send the complete request in ONE TCP write -> receive ->
// status code + body position. Speed:
//   - several stages run in the SAME task cycle where the library allows it
//     (connected -> send at once, sent -> read at once, complete -> parse)
//   - the answer is complete as soon as Content-Length bytes of body arrived
//     (the wdx interface always sends Content-Length); otherwise when the
//     server closes the connection, or 20 ms after the last data
//
// Gateway rules this FB respects:
//   - the gateway reads ONCE: the request must be one TCP write
//   - one client at a time: never run two transactions in parallel
//
// Usage: set pRequest/udiRequestLen, rising edge on xExecute, wait for xDone
//        or xError, then xExecute := FALSE (a FALSE->TRUE in the same cycle
//        starts the next request immediately).
// ===========================================================================
FUNCTION_BLOCK FB_HttpTransaction
VAR_INPUT
    xExecute      : BOOL;
    sHost         : STRING(15) := GVL_KbusRest.gc_sGatewayHost;
    uiPort        : UINT       := GVL_KbusRest.gc_uiGatewayPort;
    pRequest      : POINTER TO BYTE;
    udiRequestLen : UDINT;
    tTimeout      : TIME := T#1S;
END_VAR
VAR_OUTPUT
    xBusy         : BOOL;
    xDone         : BOOL;                                // valid HTTP answer (any status)
    xError        : BOOL;                                // no valid answer
    iStatus       : INT;                                 // 200, 204, 4xx; 0 = no answer
    pBody         : POINTER TO BYTE;
    udiBodyLen    : UDINT;
    sErrorText    : STRING(80);
END_VAR
VAR
    fbConn        : NBS.TCP_Connection;
    fbWrite       : NBS.TCP_Write;
    fbRead        : NBS.TCP_Read;
    ipAddr        : NBS.IP_ADDR;
    rtExecute     : R_TRIG;
    fbTimeout     : TON;
    fbIdle        : TON;
    iState        : INT;
    iPrev         : INT;
    iLoop         : INT;
    abChunk       : ARRAY[0..1023] OF BYTE;
    abRx          : ARRAY[0..8191] OF BYTE;              // 8 KB: a full WDA answer with stringValue and links
    udiRx         : UDINT;
    diHdrEnd      : DINT;                                // position of CR LF CR LF, -1 = not yet
    diCLen        : DINT;                                // Content-Length, -1 = none
    diPos         : DINT;
    sHead         : STRING(80);
    pHead         : POINTER TO BYTE;
    i             : UDINT;
    udiMax        : UDINT;
    xComplete     : BOOL;
END_VAR

// --- implementation --------------------------------------------------------
rtExecute(CLK := xExecute);

// up to 4 stages per call: no task cycle is wasted between finished stages
FOR iLoop := 1 TO 4 DO
    iPrev := iState;
    CASE iState OF
    0:  // idle
        IF rtExecute.Q THEN
            xBusy := TRUE; xDone := FALSE; xError := FALSE;
            iStatus := 0; pBody := ADR(abRx); udiBodyLen := 0; sErrorText := '';
            udiRx := 0; diHdrEnd := -1; diCLen := -1;
            ipAddr.sAddr := sHost;
            IF (pRequest = 0) OR (udiRequestLen = 0) THEN
                sErrorText := 'no request';
                iState := 99;
            ELSE
                iState := 10;
            END_IF
        END_IF

    10: // connect
        fbConn(xEnable := TRUE, ipAddr := ipAddr, uiPort := uiPort);
        IF fbConn.xActive THEN
            iState := 20;
        ELSIF fbConn.xError THEN
            sErrorText := 'connect failed (gateway running? port?)';
            iState := 99;
        END_IF

    20: // send the whole request in ONE write
        fbConn(xEnable := TRUE, ipAddr := ipAddr, uiPort := uiPort);
        fbWrite(xExecute := TRUE, hConnection := fbConn.hConnection,
                szSize := udiRequestLen, pData := pRequest);
        IF fbWrite.xDone THEN
            fbWrite(xExecute := FALSE);
            iState := 30;
        ELSIF fbWrite.xError THEN
            fbWrite(xExecute := FALSE);
            sErrorText := 'send failed';
            iState := 99;
        END_IF

    30: // receive
        fbConn(xEnable := TRUE, ipAddr := ipAddr, uiPort := uiPort);
        fbRead(xEnable := TRUE, hConnection := fbConn.hConnection,
               szSize := SIZEOF(abChunk), pData := ADR(abChunk));
        IF fbRead.xReady AND fbRead.szCount > 0 THEN
            FOR i := 0 TO TO_UDINT(fbRead.szCount) - 1 DO   // szCount is CAA.SIZE (64 bit on ARM64)
                IF udiRx < SIZEOF(abRx) THEN
                    abRx[udiRx] := abChunk[i];
                    udiRx := udiRx + 1;
                END_IF
            END_FOR
            fbIdle(IN := FALSE);
            // header complete? then read Content-Length once
            IF diHdrEnd < 0 THEN
                diHdrEnd := F_FindBytes(ADR(abRx), udiRx, 0, '$R$N$R$N');
                IF diHdrEnd >= 0 THEN
                    diPos := F_FindBytes(ADR(abRx), TO_UDINT(diHdrEnd), 0, 'Content-Length: ');
                    IF diPos >= 0 THEN
                        diCLen := 0;
                        i := TO_UDINT(diPos) + 16;
                        WHILE (i < udiRx) AND (abRx[i] >= 48) AND (abRx[i] <= 57) DO
                            diCLen := diCLen * 10 + (abRx[i] - 48);
                            i := i + 1;
                        END_WHILE
                    END_IF
                END_IF
            END_IF
        END_IF
        xComplete := (diHdrEnd >= 0) AND (diCLen >= 0) AND (TO_DINT(udiRx) >= diHdrEnd + 4 + diCLen);
        fbIdle(IN := udiRx > 0, PT := T#20MS);
        IF xComplete OR ((udiRx > 0) AND (fbRead.xError OR NOT fbConn.xActive OR fbIdle.Q)) THEN
            iState := 40;
        ELSIF (udiRx = 0) AND (fbRead.xError OR NOT fbConn.xActive) THEN
            sErrorText := 'connection closed without answer';
            iState := 99;
        END_IF

    40: // parse status line and body position, close
        udiMax := MIN(udiRx, 80);
        pHead := ADR(sHead);
        FOR i := 0 TO udiMax - 1 DO
            pHead[i] := abRx[i];
        END_FOR
        pHead[udiMax] := 0;
        IF FIND(sHead, 'HTTP/1.') = 1 AND LEN(sHead) >= 12 THEN
            iStatus := STRING_TO_INT(MID(sHead, 3, 10));    // 'HTTP/1.0 200 OK' -> '200'
        END_IF
        IF diHdrEnd < 0 THEN
            diHdrEnd := F_FindBytes(ADR(abRx), udiRx, 0, '$R$N$R$N');
        END_IF
        IF diHdrEnd >= 0 THEN
            pBody := ADR(abRx[diHdrEnd + 4]);
            udiBodyLen := udiRx - TO_UDINT(diHdrEnd + 4);
        END_IF
        fbRead(xEnable := FALSE, hConnection := fbConn.hConnection);
        fbConn(xEnable := FALSE);
        fbIdle(IN := FALSE);
        xBusy := FALSE;
        IF iStatus > 0 THEN
            xDone := TRUE;
        ELSE
            xError := TRUE;
            sErrorText := 'no valid HTTP answer';
        END_IF
        iState := 0;

    99: // error: close everything
        fbWrite(xExecute := FALSE);
        fbRead(xEnable := FALSE, hConnection := fbConn.hConnection);
        fbConn(xEnable := FALSE);
        fbIdle(IN := FALSE);
        xBusy := FALSE;
        xError := TRUE;
        iState := 0;
    END_CASE

    // stop when nothing changed (waiting for the network) or finished
    IF (iState = iPrev) OR (iState = 0) THEN
        EXIT;
    END_IF
END_FOR

// overall time-out (also releases the gateway, which serves one client at a time)
fbTimeout(IN := (iState >= 10) AND (iState < 40), PT := tTimeout);
IF fbTimeout.Q THEN
    sErrorText := 'timeout';
    fbWrite(xExecute := FALSE);
    fbRead(xEnable := FALSE, hConnection := fbConn.hConnection);
    fbConn(xEnable := FALSE);
    xBusy := FALSE;
    xError := TRUE;
    iState := 0;
END_IF
