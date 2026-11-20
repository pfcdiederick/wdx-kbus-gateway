// ===========================================================================
// PLC_PRG  -  example: running light on the 8 DO via the wdx data layer,
//             a text for the containers (register Codesys/Message) and the
//             answer of the Python container (register Python/Response)
// Object type in CODESYS: POU, Program, language ST
// Task: cyclic, 10 ms (MainTask)
//
// WARNING: this switches REAL outputs. Check what is connected to DO1..DO8
// before you start the application.
// ===========================================================================
PROGRAM PLC_PRG
VAR
    fbIo        : FB_WdxKbusIo;
    xRun        : BOOL := FALSE;        // set TRUE online to start the running light
    tStep       : TIME := T#500MS;
    fbStep      : TON;
    uiStep      : UINT := 1;
    i           : UINT;
    fbText      : FB_WdxWriteString;    // text -> register Codesys/Message (declare it in the register file)
    sText       : STRING(127) := 'Hello from CODESYS';   // change it online: the container prints it
    fbAnswer    : FB_WdxReadString;     // register Python/Response, read every 200 ms
    sAnswer     : STRING(127);          // last answer of the Python container
    udiAnswers  : UDINT;                // number of new answers
END_VAR

// --- implementation --------------------------------------------------------
fbStep(IN := xRun AND NOT fbStep.Q, PT := tStep);
IF fbStep.Q THEN
    uiStep := (uiStep MOD 8) + 1;
END_IF
FOR i := 1 TO 8 DO
    fbIo.axDOSet[i] := xRun AND (i = uiStep);
END_FOR

// KBus I/O through the wdx data layer: PATCH DO array + one GET per exchange
fbIo(xEnable := TRUE, uiNumDI := 0, uiNumDO := 8, uiNumAI := 0, uiNumAO := 0);

// fbIo.axDO[n] confirmed outputs, fbIo.axDI[n] / fbIo.auiAI[n] inputs
// fbIo.tLastExchange, fbIo.udiExchanges, fbIo.udiErrors, fbIo.iLastStatus, fbIo.sLastError

// Text for the containers: written automatically whenever sText changes
fbText(sValue := sText);
// fbText.xDone / fbText.xError, fbText.iStatus, fbText.sError, fbText.sWritten

// Answer of the Python container (respond() in example.py): 'ping' -> 'pong',
// 'time' -> date and time, anything else -> 'OK hh:mm:ss: <text>'
fbAnswer(xEnable := TRUE);
IF fbAnswer.xNew THEN
    sAnswer := fbAnswer.sValue;
    udiAnswers := udiAnswers + 1;
END_IF
// fbAnswer.xValid, fbAnswer.iStatus, fbAnswer.sError
