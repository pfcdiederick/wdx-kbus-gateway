// ===========================================================================
// PRG_WdaSerial_Example  -  COM1 through FB_WdaSerial
// Object type in CODESYS: POU, Program, language ST
// Task: cyclic, e.g. 10 ms
//
// Sends sRequest on a rising edge of xSendRequest (e.g. set online) and
// every tCyclic when xCyclic is TRUE; every received frame is copied to
// sLastFrame. The COM1 settings are set here (fbCom1.udiSetBaudrate /
// sSetFormat / uiSetFrameGap, 0 / '' = keep) and shown in fbCom1.udiBaudrate / sFormat.
// ===========================================================================
PROGRAM PRG_WdaSerial_Example
VAR
    fbCom1        : FB_WdaSerial;
    sRequest      : STRING(255) := 'STATUS?$R$N';   // what the device expects
    xSendRequest  : BOOL;                           // set TRUE online: send once
    xCyclic       : BOOL;                           // TRUE: send every tCyclic
    tCyclic       : TIME := T#1S;
    fbCyclic      : TON;
    sLastFrame    : STRING(255);
    udiFrames     : UDINT;
END_VAR

// --- implementation --------------------------------------------------------
fbCyclic(IN := xCyclic AND NOT fbCyclic.Q, PT := tCyclic);

fbCom1(xEnable := TRUE,
       sTxData := sRequest,
       xSend := xSendRequest OR fbCyclic.Q);
IF fbCom1.xSendDone THEN
    xSendRequest := FALSE;
END_IF

IF fbCom1.xNewFrame THEN
    sLastFrame := fbCom1.sFrame;                    // binary: fbCom1.abFrame[0..uiFrameLen-1]
    udiFrames := udiFrames + 1;
END_IF

// fbCom1.xOpen, fbCom1.udiBaudrate, fbCom1.sFormat, fbCom1.uiFrameGap   settings / state
// fbCom1.xOnline, fbCom1.iLastStatus, fbCom1.iLastWdaCode, fbCom1.sLastError
