// ===========================================================================
// PRG_TextExchange  -  text to the Python container and its answer
// Object type in CODESYS: POU, Program, language ST
// Uses: FB_WdxWriteString, FB_WdxReadString
// Task: add it to the call list of the existing cyclic task (e.g. MainTask,
//       10 ms) AFTER the programs that are already there. It is independent of
//       PLC_PRG and of the KBus I/O: nothing else in the project changes.
//
//   sText    -> register Codesys/Message  (written whenever sText changes)
//   sAnswer  <- register Python/Response  (read every 200 ms)
// Online: sText := 'ping'  ->  sAnswer = 'pong'
// ===========================================================================
PROGRAM PRG_TextExchange
VAR
    fbText      : FB_WdxWriteString;    // sId default 0-0-registers-codesys-message-value
    sText       : STRING(127) := 'Hello from CODESYS';
    fbAnswer    : FB_WdxReadString;     // sId default 0-0-registers-python-response-value
    sAnswer     : STRING(127);          // last answer of the Python container
    udiAnswers  : UDINT;                // number of answers received
END_VAR

// --- implementation --------------------------------------------------------
// text to the Python container: written automatically whenever sText changes
fbText(sValue := sText);

// answer of the Python container (respond() in example.py)
fbAnswer(xEnable := TRUE);
IF fbAnswer.xNew THEN
    sAnswer := fbAnswer.sValue;
    udiAnswers := udiAnswers + 1;
END_IF
// diagnostics: fbText.iStatus (204 ok), fbText.sError, fbAnswer.xValid, fbAnswer.iStatus (200 ok), fbAnswer.sError
