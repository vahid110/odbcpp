function(apply_compiler_settings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE 
      /W4 /WX
      /permissive-
      /utf-8
      # Match the non-MSVC policy for intentionally unused API parameters and
      # locals retained for protocol readability.
      /wd4100 /wd4189
    )
  else()
    target_compile_options(${target} PRIVATE 
      -Wall -Wextra -Wpedantic -Werror
      -Wno-unused-parameter
      -Wno-unused-variable
      -Wno-unused-function
    )
    
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
      target_compile_options(${target} PRIVATE -Wno-c++98-compat)
    endif()
  endif()
  
  # Debug/Release specific settings
  target_compile_definitions(${target} PRIVATE
    $<$<CONFIG:Debug>:DEBUG>
    $<$<CONFIG:Release>:NDEBUG>
  )
endfunction()
