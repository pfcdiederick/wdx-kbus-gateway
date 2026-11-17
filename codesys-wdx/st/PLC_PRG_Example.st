// ===========================================================================
// PLC_PRG  -  example: running light on the 8 DO via the wdx data layer
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
