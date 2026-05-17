# Invoked from POST_BUILD on MiniDAWLab. Copies developer-local LAME next to the executable when present.
# If LAME_SRC is missing, does nothing (configure/build always succeed).

if(NOT DEFINED LAME_SRC OR NOT DEFINED LAME_DST)
    message(FATAL_ERROR "CopyOptionalLame.cmake requires -DLAME_SRC=... and -DLAME_DST=...")
endif()

if(EXISTS "${LAME_SRC}")
    get_filename_component(_lame_dst_parent "${LAME_DST}" DIRECTORY)
    file(MAKE_DIRECTORY "${_lame_dst_parent}")

    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${LAME_SRC}" "${LAME_DST}"
        RESULT_VARIABLE _lame_copy_rc)
    if(NOT _lame_copy_rc EQUAL 0)
        message(WARNING "CopyOptionalLame.cmake: copy_if_different failed (code ${_lame_copy_rc}) for \"${LAME_SRC}\" -> \"${LAME_DST}\"")
    endif()
endif()
