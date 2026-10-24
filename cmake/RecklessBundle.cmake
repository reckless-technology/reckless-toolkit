# What a bundle carries beside its module: the icon - Icon.icns in a macOS bundle's Resources, which its plist names,
# and on Windows an .ico in the module's resources and, for a VST3, Plugin.ico beside a desktop.ini in the bundle's
# folder, the VST3 convention for a bundle's icon in Explorer - the PkgInfo a macOS bundle holds, the Windows version
# resource, and on macOS an ad-hoc signature over the whole bundle, made once everything is in it. A product's format
# recipes make their bundles with these, on the product's identity: the variables each function names.
include_guard(GLOBAL)

# The picture every bundle's icon is rendered from, 1024 px with transparency: the company's 3D logo unless a product
# names its own.
get_filename_component(_reckless_default_icon "${CMAKE_CURRENT_LIST_DIR}/../assets/logos/RecklessAppIcon.png" ABSOLUTE)
set(RECKLESS_BUNDLE_ICON "${_reckless_default_icon}" CACHE FILEPATH
    "The 1024 px picture every bundle's icon is made from")

# reckless_bundle_icon(<target> <file name> <out var>)
#   The icon (RECKLESS_BUNDLE_ICON, the company's 3D logo by default) rendered for <target> as <file name> - an .icns or
#   an .ico, by the name's extension (app/make_app_icons.py, run by Python3_EXECUTABLE with Pillow) - into the target's
#   own folder of the build tree, by a custom command of the calling directory that runs again only when the picture or
#   the script changes; its path in <out var>.
function(reckless_bundle_icon target file out)
    set(source "${RECKLESS_BUNDLE_ICON}")
    get_filename_component(script "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../app/make_app_icons.py" ABSOLUTE)
    set(icon "${CMAKE_CURRENT_BINARY_DIR}/${target}/${file}")
    add_custom_command(OUTPUT "${icon}"
        COMMAND "${Python3_EXECUTABLE}" "${script}" "${source}" "${icon}"
        DEPENDS "${source}" "${script}"
        COMMENT "The series' icon for ${target}: ${file}"
        VERBATIM)
    set(${out} "${icon}" PARENT_SCOPE)
endfunction()

# reckless_mac_bundle_files(<target> <package type>)
#   A macOS bundle's Icon.icns in its Resources and its PkgInfo - the package type (BNDL for a plug-in, APPL for an app)
#   and the signature ???? - beside its Info.plist.
function(reckless_mac_bundle_files target type)
    reckless_bundle_icon(${target} Icon.icns icns)
    set(pkginfo "${CMAKE_CURRENT_BINARY_DIR}/${target}/PkgInfo")
    file(CONFIGURE OUTPUT "${pkginfo}" CONTENT "${type}????")
    target_sources(${target} PRIVATE "${icns}" "${pkginfo}")
    set_source_files_properties("${icns}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    set_source_files_properties("${pkginfo}" PROPERTIES MACOSX_PACKAGE_LOCATION ".")
endfunction()

# reckless_windows_version_resource(<target> <identity prefix> <DLL|APP> <file name> <icon>)
#   A Windows module's or executable's version resource (VERSIONINFO, RecklessVersion.rc.in beside this file) - what
#   Explorer's Properties show and a host's scan may read: the file and product versions as four numbers, the product's
#   version MAJOR,MINOR,BUILD,0 (RECKLESS_VERSION and RECKLESS_VERSION_MAJOR, _MINOR and _BUILD); the file type, a DLL
#   (a plug-in module) or an APP (an app); and, in US English, the company, the display name as the file's description
#   and the product, the version as text, the copyright and the file's name - with the icon at the given path as icon 1,
#   which an app's windows load. The identity is the variables <prefix>_COMPANY, <prefix>_DISPLAY and
#   <prefix>_COPYRIGHT.
function(reckless_windows_version_resource target prefix type file icon)
    set(VERSION "${RECKLESS_VERSION}")
    set(FILE_VERSION "${RECKLESS_VERSION_MAJOR},${RECKLESS_VERSION_MINOR},${RECKLESS_VERSION_BUILD},0")
    set(FILE_TYPE "VFT_${type}")
    set(COMPANY "${${prefix}_COMPANY}")
    set(DESCRIPTION "${${prefix}_DISPLAY}")
    set(COPYRIGHT "${${prefix}_COPYRIGHT}")
    set(ORIGINAL_FILENAME "${file}")
    set(ICON "${icon}")
    set(rc "${CMAKE_CURRENT_BINARY_DIR}/${target}/Version.rc")
    configure_file("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/RecklessVersion.rc.in" "${rc}" @ONLY)
    target_sources(${target} PRIVATE "${rc}" "${icon}")
    set_source_files_properties("${rc}" PROPERTIES OBJECT_DEPENDS "${icon}")
endfunction()

# reckless_sign_bundle(<target> <bundle>)
#   On macOS, an ad-hoc signature over the whole bundle once <target> is linked - after anything the target's other
#   post-build steps write into it (moduleinfo.json), since the signature seals the bundle's files - so codesign
#   --verify --deep --strict holds it, where the linker's signature covers the binary alone. Nothing elsewhere.
function(reckless_sign_bundle target bundle)
    if(APPLE)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND codesign --force --sign - "${bundle}"
            VERBATIM)
    endif()
endfunction()
