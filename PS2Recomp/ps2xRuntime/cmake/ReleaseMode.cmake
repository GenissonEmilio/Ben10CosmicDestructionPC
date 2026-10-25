include(CheckIPOSupported)

check_ipo_supported(RESULT IPO_SUPPORTED OUTPUT IPO_ERROR)

# RelWithDebInfo gets the same flags as Release so profiling builds (with a PDB) run at full speed.
function(EnableFastReleaseMode TargetName)
    message("> Enabling optimization for: ${TargetName}")
    if(MSVC)
        target_compile_options(${TargetName} PRIVATE
            $<$<CONFIG:Release,RelWithDebInfo>:
                /O2 # speed
                /Ob2 # inline aggressively
                /Oi # intrinsics
                /Gy # function-level linking
                /Gw # global data in COMDAT
                /GF # string pooling
                /Zc:inline # remove unreferenced inline
                /fp:fast # fast math (graphics friendly)
                /DNDEBUG
                /arch:AVX2 # Advanced Vector Extensions 2
                /GS- # Disable Buffer Security Check (faster)
                /Qspectre- # Disable Spectre mitigations (faster)
            >
        )

        if(TARGET ${TargetName})
            target_link_options(${TargetName} PRIVATE
                $<$<CONFIG:Release,RelWithDebInfo>:
                    /OPT:REF # remove unreferenced
                    /OPT:ICF # fold identical COMDATs
                >
            )
        endif()
    endif()

    # No /GL, /LTCG or IPO: whole-program optimization over ~6,400 generated files makes the link
    # impractical. Each file is still built with /O2 (same as KIexster/god-of-war-recomp's build script).
endfunction()