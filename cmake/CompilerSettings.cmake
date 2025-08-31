function(apply_compiler_settings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE 
      /W4 /WX
      /permissive-
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