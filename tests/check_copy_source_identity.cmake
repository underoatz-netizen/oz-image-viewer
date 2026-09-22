# check_copy_source_identity.cmake - source-scan CTest for the Stage 9.4
# copy-source identity contract.
#
# When editing is compiled out the interactive copy path still builds an
# edit_source_buffer from the decoded full-resolution pixels. That buffer must
# carry a valid (nonzero) document identity, because edit_source_buffer::
# is_valid() rejects a zero id and the copy controller then fails every
# request with "no valid immutable source available for copy".
#
# The generating identity is the OFF-mode document generation: nonzero and
# stable for the lifetime of the current document, so it binds the source to
# the active document without weakening the generation-based stale-completion
# check.
#
# Usage: cmake -DMAIN_CPP=<src/viewer/main.cpp> -P check_copy_source_identity.cmake

if(NOT EXISTS "${MAIN_CPP}")
    message(FATAL_ERROR "main.cpp not found: ${MAIN_CPP}")
endif()

file(READ "${MAIN_CPP}" content)

# The interactive copy request must never be built with a zero document
# identity: edit_source_buffer::is_valid() requires a nonzero id, and a zero id
# makes the copy controller reject the source. (Struct member default
# initialisers such as "std::uint64_t document_instance_id = 0;" are fine and
# are not assignments of a live request or source.)
foreach(zero_assignment "req.document_instance_id = 0;" "->document_instance_id = 0;")
    string(FIND "${content}" "${zero_assignment}" zero_position)
    if(NOT zero_position EQUAL -1)
        message(FATAL_ERROR
                "main.cpp assigns ${zero_assignment} editing-OFF copy would "
                "reject every full-resolution source in edit_source_buffer::is_valid()")
    endif()
endforeach()

# The editing-OFF copy path must bind the source to the current document
# generation.
set(off_anchor "Editing OFF: full-resolution source with orientation applied")
string(FIND "${content}" "${off_anchor}" off_position)
if(off_position EQUAL -1)
    message(FATAL_ERROR "editing-OFF copy construction block not found in main.cpp")
endif()

math(EXPR off_window_length "700")
string(SUBSTRING "${content}" ${off_position} ${off_window_length} off_window)

string(FIND "${off_window}" "req.document_instance_id = app.doc.generation;" off_request_identity)
if(off_request_identity EQUAL -1)
    message(FATAL_ERROR
            "editing-OFF copy request does not bind document_instance_id to app.doc.generation")
endif()

string(FIND "${off_window}" "buf->document_instance_id = req.document_instance_id;" off_buffer_identity)
if(off_buffer_identity EQUAL -1)
    message(FATAL_ERROR
            "editing-OFF copy source buffer does not inherit the request document_instance_id")
endif()

message(STATUS "check_copy_source_identity: OK (editing-OFF copy source carries a nonzero document identity)")
