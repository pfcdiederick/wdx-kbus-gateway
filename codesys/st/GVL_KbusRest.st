// ===========================================================================
// GVL_KbusRest  -  constants for the kbus-gateway REST client
// Object type in CODESYS: Global Variable List  (attribute qualified_only)
// ===========================================================================
{attribute 'qualified_only'}
VAR_GLOBAL CONSTANT
    gc_sGatewayHost   : STRING(15) := '127.0.0.1';   // both containers use the host network
    gc_uiGatewayPort  : UINT       := 8000;          // HTTP_PORT of the kbus-gateway
    gc_uiMaxDigital   : UINT       := 64;            // array size for DI / DO
    gc_uiMaxAnalog    : UINT       := 32;            // array size for AI / AO
END_VAR
