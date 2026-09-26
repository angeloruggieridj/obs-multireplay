# Runs at install time (install(SCRIPT) in CMakeLists.txt), after the template
# has put the plugin where OBS 32 looks when OBS itself lives in /usr:
#   <prefix>/<libdir>/obs-plugins/<name>.so
#   <prefix>/share/obs/obs-plugins/<name>/
#
# Two facts make that incomplete, both read from the binaries of OBS's own
# Ubuntu 26.04 .debs (32.2.2 and 33.0.0-beta4), not from its documentation:
#   - OBS 33 searches <prefix>/<libdir>/obs-modules/plugins and
#     <prefix>/share/obs/obs-modules/plugins; the obs-plugins folders still load,
#     as "legacy", until OBS 34;
#   - OBS's own .deb -- the only OBS 33 published for Ubuntu 26.04 -- installs
#     under /usr/local and searches /usr/local, never /usr.
#
# So the same files are linked into the other places. OBS loads the first copy
# of a module it finds and skips the rest as duplicates. Links, not copies: one
# binary on disk, and one debug-symbol file for it in the -dbgsym package.
#
# Set by CMakeLists.txt before this runs: MR_LIBDIR (relative, e.g.
# lib/x86_64-linux-gnu), MR_DATADIR (relative, share) and MR_NAME.

function(mr_link target link)
  set(dest "$ENV{DESTDIR}${link}")
  get_filename_component(dir "${dest}" DIRECTORY)
  file(MAKE_DIRECTORY "${dir}")
  # file(REMOVE) takes the link itself, never what a directory link points at.
  if(IS_SYMLINK "${dest}" OR EXISTS "${dest}")
    file(REMOVE "${dest}")
  endif()
  file(CREATE_LINK "${target}" "${dest}" SYMBOLIC)
  message(STATUS "Installing: ${dest} -> ${target}")
endfunction()

set(_prefix "${CMAKE_INSTALL_PREFIX}")
set(_so "${MR_NAME}.so")

# OBS 33's layout, in the same prefix. Relative, so the links stay right in a
# tarball installed under any prefix.
mr_link("../../obs-plugins/${_so}" "${_prefix}/${MR_LIBDIR}/obs-modules/plugins/${_so}")
mr_link("../../obs-plugins/${MR_NAME}" "${_prefix}/${MR_DATADIR}/obs/obs-modules/plugins/${MR_NAME}")

# The /usr/local mirror, for OBS's own .deb: only when this is the system
# install a .deb makes. Any other prefix is somebody's own layout.
if(_prefix STREQUAL "/usr")
  foreach(_where IN ITEMS obs-plugins obs-modules/plugins)
    mr_link("/usr/${MR_LIBDIR}/obs-plugins/${_so}" "/usr/local/${MR_LIBDIR}/${_where}/${_so}")
    mr_link("/usr/${MR_DATADIR}/obs/obs-plugins/${MR_NAME}" "/usr/local/${MR_DATADIR}/obs/${_where}/${MR_NAME}")
  endforeach()
endif()
