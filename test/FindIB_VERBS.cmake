# - Try to find IB Verbs
# Once done this will define
#  IB_VERBS_FOUND - System has IB Verbs
#  IB_VERBS_INCLUDE_DIRS - The IB Verbs include directories
#   e.g., "/usr/include", _NOT_ "/usr/include/infiniband"
#  IB_VERBS_LIBRARIES - The libraries needed to use IB Verbs

find_path(IB_VERBS_INCLUDE_DIR infiniband/verbs.h
  HINTS /usr/local/include /usr/include)

find_library(IB_VERBS_LIBRARY NAMES ibverbs
  PATHS /usr/local/lib64 /usr/lib64 /usr/local/lib /usr/lib)

set(IB_VERBS_INCLUDE_DIRS ${IB_VERBS_INCLUDE_DIR})
set(IB_VERBS_LIBRARIES ${IB_VERBS_LIBRARY})

include(FindPackageHandleStandardArgs)
# handle the QUIETLY and REQUIRED arguments and set IB_VERBS_FOUND to TRUE
# if all listed variables are TRUE
find_package_handle_standard_args(IB_VERBS DEFAULT_MSG
                                  IB_VERBS_INCLUDE_DIR IB_VERBS_LIBRARY)

mark_as_advanced(IB_VERBS_INCLUDE_DIR IB_VERBS_LIBRARY)
