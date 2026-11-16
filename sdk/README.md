# WAGO SDK headers

`sdk/include/` holds the ADI/DAL and KBUS-info header files that the real KBUS
backend is compiled against (`sdk/include/dal/adi_application_interface.h`
must exist). The libraries are not used from here: the gateway loads the
PFC's own libraries at runtime. See README section 2.2.

**The WAGO headers are not in the git repository** (they carry a WAGO
"proprietary rights" notice), see `.gitignore`. After cloning, only
`OsCommon.h` (our own minimal stand-in) is here; import the rest as below.

Fill it in one of two ways:

    # A. from the prebuilt packages of a WAGO firmware SDK (no SDK build needed)
    tools/import-sdk-packages.sh ~/Documents/WagoFirmware/pfc300-firmware-sdk/ptxproj/configs/wago-pfc300/packages

    # B. from a completely built SDK sysroot
    tools/import-sdk-headers.sh ~/wago/ptxproj

The DAL version of the headers must match `libdal.so.<version>` on the PFC.
Without these headers only a simulation image can be built (`REQUIRE_ADI=0`).

## Current contents

Taken with method A from the WAGO PFC300 firmware SDK (FW30-V04.08.09,
`ptxproj/configs/wago-pfc300/packages/`):

- `include/dal/*` from `dal_1.0.1_arm64.tgz`
- `include/ldkc_kbus_information.h` from `libdbuskbuscommon_1.0.0.0_arm64.tgz`
- `include/OsCommon.h`: minimal stand-in (types and attribute macros only)

The headers contain no architecture-specific code. `libdal` has version 1.0.1
on both the PFC300 (arm64) and the PFC200 FW31 04.09.50 (armv7), and the
exported `ldkc_KbusInfo_*` functions are identical, so the headers match the
32-bit libraries that the gateway loads on the PFC200. Tested on a PFC200
750-8217 with FW31.
