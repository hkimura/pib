# Ack: random0, 12-11-2009
# http://www.linuxquestions.org/questions/programming-9/cmake-and-very-platform-specific-makefiles-774795/

SET( MODULE_NAME pib)

SET( MODULE_SOURCE_FILES
  pib_ah.c  pib_cq.c  pib_debugfs.c  pib_dma.c  pib_easy_sw.c  pib.h  pib_lib.c  pib_mad.c
  pib_mad.h  pib_mad_pma.c  pib_main.c  pib_mr.c  pib_multicast.c  pib_packet.h  pib_pd.c
  pib_qp.c  pib_rc.c  pib_spinlock.h  pib_srq.c  pib_thread.c  pib_trace.h  pib_ucontext.c  pib_ud.c)

SET( MODULE_OUTPUT_FILES
  pib_main.o pib_dma.o pib_lib.o
  pib_ucontext.o pib_pd.o pib_qp.o pib_multicast.o pib_cq.o pib_srq.o
  pib_ah.o pib_mr.o pib_mad.o pib_mad_pma.o pib_easy_sw.o
  pib_thread.o pib_ud.o pib_rc.o pib_debugfs.o)

SET( MODULE_FILE
  ${MODULE_NAME}.ko
)

SET( MODULE_OUTPUT_DIR
  ${CMAKE_CURRENT_BINARY_DIR}
#  ${CMAKE_CURRENT_BINARY_DIR}/modules/${MODULE_NAME}
)

SET( KERNEL_DIR
  "/lib/modules/${CMAKE_SYSTEM_VERSION}/build"
)

# This requires cmake 2.8.5.
# https://public.kitware.com/Bug/bug_relationship_graph.php?bug_id=11302&graph=relation
include(ProcessorCount)
ProcessorCount(N)
if(N GREATER 1)
  message(STATUS "Lucky you! You have ${N} cores. We'll parallelize kbuild.")
  set(parallel_flag "-j${N}")
endif()

### >>> Construct Kbuild file
# kbuild is independent from cmake/make, thus we have to explicitly say
# whether want "-DDEBUG" in the kbuild file.
# This kicks in only when the user specifies -DCMAKE_BUILD_TYPE=Debug
string(TOLOWER "${CMAKE_BUILD_TYPE}" build_type_lower)
if (build_type_lower STREQUAL "debug")
  set(kbuild_in_ddebug "-DDEBUG")
else (build_type_lower STREQUAL "debug")
  set(kbuild_in_ddebug "")
endif (build_type_lower STREQUAL "debug")

set(kbuild_in_outputs_space_separated "")
set(additional_clean_cmd_files "")
FOREACH( MODULE_OUTPUT_FILE ${MODULE_OUTPUT_FILES} )
  set(kbuild_in_outputs_space_separated
    "${kbuild_in_outputs_space_separated} ${MODULE_OUTPUT_FILE}")
  set(additional_clean_cmd_files
    ${additional_clean_cmd_files} ".${MODULE_OUTPUT_FILE}.cmd")
ENDFOREACH( MODULE_OUTPUT_FILE )

CONFIGURE_FILE(Kbuild.in ${MODULE_OUTPUT_DIR}/Kbuild)
### <<< Construct Kbuild file

FOREACH( MODULE_SOURCE_FILE ${MODULE_SOURCE_FILES} )
  CONFIGURE_FILE(
    ${MODULE_SOURCE_FILE}
    ${MODULE_OUTPUT_DIR}/${MODULE_SOURCE_FILE}
    COPYONLY
  )
ENDFOREACH( MODULE_SOURCE_FILE )

ADD_CUSTOM_COMMAND(
  OUTPUT ${MODULE_OUTPUT_DIR}/${MODULE_FILE}
  COMMAND ${KBUILD_COMMAND}
  WORKING_DIRECTORY ${MODULE_OUTPUT_DIR}
  DEPENDS ${MODULE_SOURCE_FILES} Kbuild.in
  VERBATIM
)

SET( KBUILD_COMMAND
  ${CMAKE_MAKE_PROGRAM} -C ${KERNEL_DIR} M=${MODULE_OUTPUT_DIR} ${parallel_flag} modules
)
ADD_CUSTOM_TARGET(
  ${MODULE_NAME}
  ALL
  DEPENDS ${MODULE_OUTPUT_DIR}/${MODULE_FILE}
)

# To install the compiled .ko file,
SET( KBUILD_INSTALL_COMMAND
  sudo ${CMAKE_MAKE_PROGRAM} -C ${KERNEL_DIR} M=${MODULE_OUTPUT_DIR} ${parallel_flag} modules_install
)
add_custom_target(modules_install
  COMMAND ${KBUILD_INSTALL_COMMAND}
  WORKING_DIRECTORY ${MODULE_OUTPUT_DIR}
  DEPENDS ${MODULE_NAME}
  COMMENT "This command installs the compiled .ko file to the kernel. **You must be a sudoer**"
  VERBATIM
)

### >>> customize "make clean"
# again because kbuild is separate from cmake/make...
set(additional_clean_files
  ${MODULE_OUTPUT_FILES}
  ${MODULE_SOURCE_FILES}
  ${MODULE_FILE}
  Kbuild
  ${MODULE_NAME}.o
  .${MODULE_NAME}.o.cmd
  .${MODULE_FILE}.cmd
  ${MODULE_NAME}.mod.c
  ${MODULE_NAME}.mod.o
  .${MODULE_NAME}.mod.o.cmd
  .tmp_versions
  modules.order
  Module.symvers
  Module.markers
  ${additional_clean_cmd_files})

set_directory_properties(PROPERTIES
  ADDITIONAL_MAKE_CLEAN_FILES "${additional_clean_files}")
### <<< customize "make clean"
