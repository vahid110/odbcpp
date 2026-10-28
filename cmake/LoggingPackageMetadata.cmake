# Configure-time inputs for Windows package notices/version reporting, not proof
# of which headers compiled an existing binary. Keep local paths out of payloads.
function(odbcpp_write_logging_package_metadata source_dir output)
  file(REAL_PATH "${source_dir}" _source)
  string(REPLACE "\\" "\\\\" _quoted_source "${_source}")
  string(REPLACE "\"" "\\\"" _quoted_source "${_quoted_source}")
  string(REPLACE "\n" "\\n" _quoted_source "${_quoted_source}")
  string(REPLACE "\r" "\\r" _quoted_source "${_quoted_source}")
  string(REPLACE "\t" "\\t" _quoted_source "${_quoted_source}")
  set(_metadata "{\"schemaVersion\":1,\"configurationEvidenceOnly\":true,\"files\":{}}")
  string(JSON _metadata SET "${_metadata}" sourceDirectory "\"${_quoted_source}\"")
  foreach(_file IN ITEMS LICENSE include/spdlog/version.h
      include/spdlog/fmt/bundled/fmt.license.rst include/spdlog/fmt/bundled/base.h)
    if(NOT EXISTS "${_source}/${_file}" OR IS_DIRECTORY "${_source}/${_file}")
      # Minimal/non-packaging source inputs may still configure. A null hash
      # records their absence; the package reader refuses them before staging.
      string(JSON _metadata SET "${_metadata}" files "${_file}" null)
    else()
      file(SHA256 "${_source}/${_file}" _hash)
      string(JSON _metadata SET "${_metadata}" files "${_file}" "\"${_hash}\"")
    endif()
  endforeach()
  file(WRITE "${output}" "${_metadata}\n")
endfunction()
