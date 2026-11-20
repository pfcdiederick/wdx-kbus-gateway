// ===========================================================================
// FB_WdxWriteString  -  write a STRING to a register of the kbus-gateway
// Object type in CODESYS: POU, Function Block, language ST
// Uses: FB_HttpTransaction, F_AppendStr, F_FindBytes, GVL_KbusRest
// Needs kbus-gateway 2.2.0 or newer (registers).
//
// The register must be declared in /etc/kbus-gateway-registers.conf on the
// controller, e.g.
//     Codesys/Message   string   ""
// wdx id: 0-0-registers-codesys-message-value. Containers read it with
//     wdx.get("0-0-registers-codesys-message-value")       (wdx_client.py)
//
// One request: PATCH /wda/parameters/<sId> with Content-Length -> 204.
// The text is sent as a JSON string: " and \ are escaped, control characters
// and all bytes from 16#7F up as \u00XX. CODESYS strings are single-byte
// (Latin-1), so e.g. 'Temperatur 21 °C' arrives in the gateway and in Python
// as correct UTF-8. The gateway keeps at most 127 bytes UTF-8: a longer text
// (or one with many characters above 16#7F, 2 bytes each) is rejected with
// HTTP 400 (WDA code 39) and nothing is written.
//
// Usage
//   xOnChange = TRUE : writes automatically whenever sValue changes
//                      (after an error: again after tRetry)
//   xExecute         : rising edge = write sValue now (also when unchanged,
//                      e.g. after a restart of the gateway)
//   xDone            : the last write was accepted (sWritten = that text)
//   xError           : the last write failed, see iStatus / sError
// Runs in parallel with FB_WdxKbusIo: each FB has its own connection.
// ===========================================================================
FUNCTION_BLOCK FB_WdxWriteString
VAR_INPUT
    xExecute      : BOOL;
    xOnChange     : BOOL := TRUE;
    sId           : STRING(80) := '0-0-registers-codesys-message-value';
    sValue        : STRING(127);
    sHost         : STRING(15) := GVL_KbusRest.gc_sGatewayHost;
    uiPort        : UINT       := GVL_KbusRest.gc_uiGatewayPort;
    tTimeout      : TIME := T#500MS;
    tRetry        : TIME := T#2S;
END_VAR
VAR_OUTPUT
    xBusy         : BOOL;
    xDone         : BOOL;
    xError        : BOOL;
    iStatus       : INT;                                 // 204 = written; 404 unknown id, 400 too long, 0 no answer
    sError        : STRING(80);
    sWritten      : STRING(127);                         // last text the gateway accepted
    udiWrites     : UDINT;
    udiErrors     : UDINT;
END_VAR
VAR
    fbHttp        : FB_HttpTransaction;
    rtExecute     : R_TRIG;
    fbRetry       : TON;
    xPending      : BOOL;
    iState        : INT;                                 // 0 idle, 1 request running
    sSending      : STRING(127);
    abBody        : ARRAY[0..1023] OF BYTE;              // 127 x 6 bytes (\u00XX) + JSON frame
    udiBody       : UDINT;
    abTx          : ARRAY[0..1279] OF BYTE;
    udiTx         : UDINT;
    sHex          : STRING(16) := '0123456789abcdef';
    pS            : POINTER TO BYTE;
    pHex          : POINTER TO BYTE;
    b             : BYTE;
    i             : UDINT;
    diPos         : DINT;
    udiEnd        : UDINT;
    pErr          : POINTER TO BYTE;
END_VAR

// --- implementation --------------------------------------------------------
rtExecute(CLK := xExecute);
IF rtExecute.Q THEN
    xPending := TRUE;
END_IF
fbRetry(IN := xError AND (iState = 0), PT := tRetry);
IF xOnChange AND (sValue <> sWritten) AND (NOT xError OR fbRetry.Q) THEN
    xPending := TRUE;
END_IF

// ---------------------------------------------------------------- start a write
IF (iState = 0) AND xPending THEN
    xPending := FALSE;
    sSending := sValue;
    IF LEN(sId) = 0 THEN
        xError := TRUE; xDone := FALSE; iStatus := 0; sError := 'sId is empty';
        udiErrors := udiErrors + 1;
    ELSE
        // body: {"data":{"type":"parameters","id":"<sId>","attributes":{"value":"<text>"}}}
        udiBody := 0;
        F_AppendStr(ADR(abBody), SIZEOF(abBody), '{"data":{"type":"parameters","id":"', udiBody);
        F_AppendStr(ADR(abBody), SIZEOF(abBody), sId, udiBody);
        F_AppendStr(ADR(abBody), SIZEOF(abBody), '","attributes":{"value":"', udiBody);
        pS := ADR(sSending);
        pHex := ADR(sHex);
        i := 0;
        WHILE (i < 127) AND (pS[i] <> 0) DO
            b := pS[i];
            IF (b = 16#22) OR (b = 16#5C) THEN                   // " and \
                abBody[udiBody] := 16#5C;
                abBody[udiBody + 1] := b;
                udiBody := udiBody + 2;
            ELSIF (b < 16#20) OR (b >= 16#7F) THEN               // control characters, Latin-1
                F_AppendStr(ADR(abBody), SIZEOF(abBody), '\u00', udiBody);
                abBody[udiBody] := pHex[SHR(b, 4)];
                abBody[udiBody + 1] := pHex[b AND 16#0F];
                udiBody := udiBody + 2;
            ELSE
                abBody[udiBody] := b;
                udiBody := udiBody + 1;
            END_IF
            i := i + 1;
        END_WHILE
        F_AppendStr(ADR(abBody), SIZEOF(abBody), '"}}}', udiBody);

        // request: header with Content-Length + body, sent in ONE TCP write
        udiTx := 0;
        F_AppendStr(ADR(abTx), SIZEOF(abTx), 'PATCH /wda/parameters/', udiTx);
        F_AppendStr(ADR(abTx), SIZEOF(abTx), sId, udiTx);
        F_AppendStr(ADR(abTx), SIZEOF(abTx), ' HTTP/1.0$R$NContent-Type: application/vnd.api+json$R$N', udiTx);
        F_AppendStr(ADR(abTx), SIZEOF(abTx), 'Content-Length: ', udiTx);
        F_AppendStr(ADR(abTx), SIZEOF(abTx), UDINT_TO_STRING(udiBody), udiTx);
        F_AppendStr(ADR(abTx), SIZEOF(abTx), '$R$N$R$N', udiTx);
        FOR i := 0 TO udiBody - 1 DO
            abTx[udiTx] := abBody[i];
            udiTx := udiTx + 1;
        END_FOR

        fbHttp(xExecute := FALSE);
        fbHttp(xExecute := TRUE, sHost := sHost, uiPort := uiPort,
               pRequest := ADR(abTx), udiRequestLen := udiTx, tTimeout := tTimeout);
        xBusy := TRUE;
        iState := 1;
    END_IF
ELSIF iState = 1 THEN
    fbHttp(xExecute := TRUE, sHost := sHost, uiPort := uiPort,
           pRequest := ADR(abTx), udiRequestLen := udiTx, tTimeout := tTimeout);
END_IF

// ---------------------------------------------------------------- result
IF (iState = 1) AND (fbHttp.xDone OR fbHttp.xError) THEN
    iStatus := fbHttp.iStatus;
    IF fbHttp.xDone AND (fbHttp.iStatus = 204) THEN
        xDone := TRUE; xError := FALSE; sError := '';
        sWritten := sSending;
        udiWrites := udiWrites + 1;
    ELSE
        xDone := FALSE; xError := TRUE;
        udiErrors := udiErrors + 1;
        IF fbHttp.xError THEN
            sError := fbHttp.sErrorText;
        ELSE
            // JSON:API error: take the text of "detail":"..."
            sError := CONCAT('HTTP ', INT_TO_STRING(fbHttp.iStatus));
            diPos := F_FindBytes(fbHttp.pBody, fbHttp.udiBodyLen, 0, '"detail":"');
            IF diPos >= 0 THEN
                udiEnd := TO_UDINT(diPos) + 10;
                i := udiEnd;
                WHILE (i < fbHttp.udiBodyLen) AND (fbHttp.pBody[i] <> 16#22) AND (i < udiEnd + 70) DO
                    i := i + 1;
                END_WHILE
                pErr := ADR(sError);
                sError := CONCAT(sError, ': ');
                udiEnd := TO_UDINT(LEN(sError));
                WHILE (udiEnd < 80) AND (TO_UDINT(diPos) + 10 < i) DO
                    pErr[udiEnd] := fbHttp.pBody[TO_UDINT(diPos) + 10];
                    udiEnd := udiEnd + 1;
                    diPos := diPos + 1;
                END_WHILE
                pErr[udiEnd] := 0;
            END_IF
        END_IF
    END_IF
    fbHttp(xExecute := FALSE);
    xBusy := FALSE;
    iState := 0;
END_IF
