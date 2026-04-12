function(set_project_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
        target_compile_definitions(${target} PRIVATE 
            _SILENCE_ALL_CXX20_DEPRECATION_WARNINGS
            _SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING
        )
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()
