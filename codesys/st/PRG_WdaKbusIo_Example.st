// ===========================================================================
// PRG_WdaKbusIo_Example  -  KBus I/O every 5 ms through FB_WdaKbusIo
// Object type in CODESYS: POU, Program, language ST
// Task: cyclic, 1 ms (e.g. task "KbusIoTask", priority 1), only this program
//
// Running light on all digital outputs, AO1 follows AI1. The channel counts
// come from the gateway (fbIo.uiNumDO, ...): no configuration needed.
//
// WARNING: this switches REAL outputs. Check what is connected to the DO and
// AO channels before you start the application.
// ===========================================================================
PROGRAM PRG_WdaKbusIo_Example
VAR
    fbIo        : FB_WdaKbusIo;
    xRun        : BOOL := FALSE;        // set TRUE online to start the running light
    tStep       : TIME := T#200MS;
    fbStep      : TON;
    uiStep      : UINT := 1;
    i           : UINT;
END_VAR

// --- implementation --------------------------------------------------------
// 1. outputs: the application writes the desired values
fbStep(IN := xRun AND NOT fbStep.Q, PT := tStep);
IF fbStep.Q AND (fbIo.uiNumDO > 0) THEN
    uiStep := (uiStep MOD fbIo.uiNumDO) + 1;
END_IF
FOR i := 1 TO fbIo.uiNumDO DO
    fbIo.axDOSet[i] := xRun AND (i = uiStep);
END_FOR
IF (fbIo.uiNumAO > 0) AND (fbIo.uiNumAI > 0) THEN
    fbIo.auiAOSet[1] := fbIo.auiAI[1];
END_IF

// 2. exchange with the gateway: every 5 ms, outputs only when they changed
fbIo(xEnable := TRUE, tInterval := T#5MS);

// 3. inputs and diagnostics
//    fbIo.axDI[n], fbIo.auiAI[n]      inputs (raw), one KBus snapshot
//    fbIo.axDO[n], fbIo.auiAO[n]      outputs as confirmed by the gateway
//    fbIo.xReady, fbIo.xOnline, fbIo.xHealthy
//    fbIo.tLastExchange, fbIo.tMaxExchange, fbIo.udiExchanges, fbIo.udiWrites
//    fbIo.udiErrors, fbIo.iLastStatus, fbIo.iLastWdaCode, fbIo.sLastError
