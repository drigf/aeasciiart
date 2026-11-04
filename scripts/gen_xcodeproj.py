#!/usr/bin/env python3
"""Regenerates AsciiDepth.xcodeproj from the files on disk.

Edit code in VS Code, add or remove files freely, then run:

    make xcodeproj        (or: python3 scripts/gen_xcodeproj.py)

Xcode picks up the new project the next time it reads it.  Every source file
under Source/ is added automatically; nothing else needs registering.
"""

import hashlib
import os
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
PROJECT_DIR = os.path.join(ROOT, "AsciiDepth.xcodeproj")
PRODUCT = "AsciiDepth"

# Files from the Adobe SDK that every effect must compile in.  They are located
# through $(AE_SDK_ROOT) (see Config/AsciiDepth.xcconfig).
SDK_SOURCES = [
    "Util/AEGP_SuiteHandler.cpp",
    "Util/MissingSuiteError.cpp",
]

FRAMEWORKS = ["CoreFoundation", "CoreGraphics", "CoreText", "CoreVideo", "Foundation"]
WEAK_FRAMEWORKS = ["Vision"]  # macOS 14+ API, weak-linked so the plug-in still loads on 13


def oid(key):
    return hashlib.md5(key.encode()).hexdigest()[:24].upper()


def q(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


SOURCE_TYPES = {
    ".cpp": "sourcecode.cpp.cpp",
    ".mm": "sourcecode.cpp.objcpp",
    ".m": "sourcecode.c.objc",
    ".c": "sourcecode.c.c",
    ".h": "sourcecode.c.h",
    ".hpp": "sourcecode.cpp.h",
    ".r": "sourcecode.rez",
    ".plist": "text.plist.xml",
    ".xcconfig": "text.xcconfig",
}
COMPILED = {".cpp", ".mm", ".m", ".c"}


def scan(subdir):
    found = []
    base = os.path.join(ROOT, subdir)
    for dirpath, dirnames, filenames in os.walk(base):
        dirnames[:] = sorted(d for d in dirnames if not d.startswith("."))
        for f in sorted(filenames):
            if os.path.splitext(f)[1] in SOURCE_TYPES:
                found.append(os.path.relpath(os.path.join(dirpath, f), ROOT))
    return found


def main():
    source_files = scan("Source")
    config_files = scan("Config")

    objects = []  # (id, comment, body)
    build_sources, build_resources, build_frameworks = [], [], []

    def add(i, comment, body):
        objects.append((i, comment, body))

    # --- file references --------------------------------------------------------------
    def file_ref(path, name=None, tree="<group>", explicit=None):
        i = oid("ref:" + path)
        ext = os.path.splitext(path)[1]
        ftype = SOURCE_TYPES.get(ext, "text")
        n = name or os.path.basename(path)
        add(i, n, "isa = PBXFileReference; lastKnownFileType = %s; name = %s; path = %s; sourceTree = %s;"
            % (ftype, q(n), q(path), q(tree)))
        return i

    # Project groups (nested by directory)
    group_children = {}  # dirpath -> list of (id, comment)

    def ensure_group(dirpath):
        if dirpath in group_children:
            return
        group_children[dirpath] = []
        if dirpath:
            parent = os.path.dirname(dirpath)
            ensure_group(parent)
            group_children[parent].append((oid("group:" + dirpath), os.path.basename(dirpath)))

    for path in source_files + config_files:
        d = os.path.dirname(path)
        ensure_group(d)
        i = oid("ref:" + path)
        ext = os.path.splitext(path)[1]
        add(i, os.path.basename(path),
            "isa = PBXFileReference; lastKnownFileType = %s; path = %s; sourceTree = \"<group>\";"
            % (SOURCE_TYPES[ext], q(os.path.basename(path))))
        group_children[d].append((i, os.path.basename(path)))
        if ext in COMPILED:
            b = oid("build:" + path)
            add(b, os.path.basename(path) + " in Sources", "isa = PBXBuildFile; fileRef = %s;" % i)
            build_sources.append((b, os.path.basename(path) + " in Sources"))
        elif ext == ".r":
            b = oid("build:" + path)
            add(b, os.path.basename(path) + " in Resources", "isa = PBXBuildFile; fileRef = %s;" % i)
            build_resources.append((b, os.path.basename(path) + " in Resources"))

    # Emit group objects (bottom-up order doesn't matter in pbxproj)
    root_id = oid("group:root")
    for dirpath, children in group_children.items():
        if dirpath == "":
            continue
        gid = oid("group:" + dirpath)
        add(gid, os.path.basename(dirpath),
            "isa = PBXGroup; children = (%s); path = %s; sourceTree = \"<group>\";"
            % ("".join("\n\t\t\t\t%s /* %s */," % c for c in children) + "\n\t\t\t", q(os.path.basename(dirpath))))

    # SDK support files
    sdk_children = []
    for rel in SDK_SOURCES:
        path = "$(AE_SDK_ROOT)/" + rel
        i = oid("ref:" + path)
        add(i, os.path.basename(rel),
            "isa = PBXFileReference; lastKnownFileType = sourcecode.cpp.cpp; name = %s; path = %s; sourceTree = \"<absolute>\";"
            % (q(os.path.basename(rel)), q(path)))
        sdk_children.append((i, os.path.basename(rel)))
        b = oid("build:" + path)
        add(b, os.path.basename(rel) + " in Sources", "isa = PBXBuildFile; fileRef = %s;" % i)
        build_sources.append((b, os.path.basename(rel) + " in Sources"))
    sdk_group = oid("group:sdk")
    add(sdk_group, "Adobe SDK",
        "isa = PBXGroup; children = (%s); name = \"Adobe SDK (compiled in)\"; sourceTree = \"<group>\";"
        % ("".join("\n\t\t\t\t%s /* %s */," % c for c in sdk_children) + "\n\t\t\t"))

    # Frameworks
    fw_children = []
    for fw, weak in [(f, False) for f in FRAMEWORKS] + [(f, True) for f in WEAK_FRAMEWORKS]:
        i = oid("fw:" + fw)
        add(i, fw + ".framework",
            "isa = PBXFileReference; lastKnownFileType = wrapper.framework; name = %s.framework; path = System/Library/Frameworks/%s.framework; sourceTree = SDKROOT;"
            % (fw, fw))
        fw_children.append((i, fw + ".framework"))
        b = oid("build:fw:" + fw)
        settings = " settings = {ATTRIBUTES = (Weak, ); };" if weak else ""
        add(b, fw + ".framework in Frameworks", "isa = PBXBuildFile; fileRef = %s;%s" % (i, settings))
        build_frameworks.append((b, fw + ".framework in Frameworks"))
    fw_group = oid("group:frameworks")
    add(fw_group, "Frameworks",
        "isa = PBXGroup; children = (%s); name = Frameworks; sourceTree = \"<group>\";"
        % ("".join("\n\t\t\t\t%s /* %s */," % c for c in fw_children) + "\n\t\t\t"))

    # Product
    product_ref = oid("product")
    add(product_ref, PRODUCT + ".plugin",
        "isa = PBXFileReference; explicitFileType = wrapper.cfbundle; includeInIndex = 0; path = %s.plugin; sourceTree = BUILT_PRODUCTS_DIR;" % PRODUCT)
    products_group = oid("group:products")
    add(products_group, "Products",
        "isa = PBXGroup; children = (\n\t\t\t\t%s /* %s.plugin */,\n\t\t\t); name = Products; sourceTree = \"<group>\";" % (product_ref, PRODUCT))

    # Root group
    root_children = list(group_children.get("", []))
    root_children_dirs = [(oid("group:" + d), os.path.basename(d)) for d in group_children if d and os.path.dirname(d) == ""]
    all_root = root_children + root_children_dirs + [(sdk_group, "Adobe SDK"), (fw_group, "Frameworks"), (products_group, "Products")]
    seen, uniq = set(), []
    for c in all_root:
        if c[0] not in seen:
            seen.add(c[0]); uniq.append(c)
    add(root_id, "Root", "isa = PBXGroup; children = (%s); sourceTree = \"<group>\";"
        % ("".join("\n\t\t\t\t%s /* %s */," % c for c in uniq) + "\n\t\t\t"))

    # --- build phases ---------------------------------------------------------------------
    res_phase, src_phase, fw_phase, script_phase = oid("phase:res"), oid("phase:src"), oid("phase:fw"), oid("phase:install")

    def files(lst):
        return "".join("\n\t\t\t\t%s /* %s */," % f for f in lst) + "\n\t\t\t"

    add(res_phase, "Resources", "isa = PBXResourcesBuildPhase; buildActionMask = 2147483647; files = (%s); runOnlyForDeploymentPostprocessing = 0;" % files(build_resources))
    add(src_phase, "Sources", "isa = PBXSourcesBuildPhase; buildActionMask = 2147483647; files = (%s); runOnlyForDeploymentPostprocessing = 0;" % files(build_sources))
    add(fw_phase, "Frameworks", "isa = PBXFrameworksBuildPhase; buildActionMask = 2147483647; files = (%s); runOnlyForDeploymentPostprocessing = 0;" % files(build_frameworks))

    script = (
        'if [ \\"${AE_INSTALL_ON_BUILD}\\" != \\"YES\\" ]; then exit 0; fi\\n'
        'DEST=\\"${AE_PLUGIN_INSTALL_DIR}\\"\\n'
        'if [ ! -w \\"$DEST\\" ]; then\\n'
        '  echo \\"warning: cannot write to $DEST - run scripts/install.sh (needs sudo) to install.\\"\\n'
        '  exit 0\\n'
        'fi\\n'
        '# The install phase runs before Xcode signs the bundle, and unsigned arm64 code will not load.\\n'
        'codesign --force --deep --sign - \\"${BUILT_PRODUCTS_DIR}/${FULL_PRODUCT_NAME}\\"\\n'
        'rm -rf \\"$DEST/${FULL_PRODUCT_NAME}\\"\\n'
        'cp -R \\"${BUILT_PRODUCTS_DIR}/${FULL_PRODUCT_NAME}\\" \\"$DEST/\\"\\n'
        'echo \\"Installed ${FULL_PRODUCT_NAME} to $DEST - restart After Effects to load it.\\"\\n'
    )
    add(script_phase, "Install into After Effects",
        "isa = PBXShellScriptBuildPhase; alwaysOutOfDate = 1; buildActionMask = 2147483647; files = (); inputPaths = (); name = \"Install into After Effects\"; outputPaths = (); runOnlyForDeploymentPostprocessing = 0; shellPath = /bin/sh; shellScript = \"%s\"; showEnvVarsInLog = 0;" % script)

    # --- target ---------------------------------------------------------------------------------
    target, proj = oid("target"), oid("project")
    proj_cfg_list, tgt_cfg_list = oid("cfglist:proj"), oid("cfglist:target")
    base_cfg = oid("ref:Config/AsciiDepth.xcconfig")

    add(target, PRODUCT,
        "isa = PBXNativeTarget; buildConfigurationList = %s; buildPhases = (\n\t\t\t\t%s /* Resources */,\n\t\t\t\t%s /* Sources */,\n\t\t\t\t%s /* Frameworks */,\n\t\t\t\t%s /* Install into After Effects */,\n\t\t\t); buildRules = (); dependencies = (); name = %s; productName = %s; productReference = %s; productType = \"com.apple.product-type.bundle\";"
        % (tgt_cfg_list, res_phase, src_phase, fw_phase, script_phase, PRODUCT, PRODUCT, product_ref))

    add(proj, "Project object",
        "isa = PBXProject; attributes = {BuildIndependentTargetsInParallel = YES; LastUpgradeCheck = 1620;}; buildConfigurationList = %s; compatibilityVersion = \"Xcode 14.0\"; developmentRegion = en; hasScannedForEncodings = 0; knownRegions = (en, Base, ); mainGroup = %s; productRefGroup = %s; projectDirPath = \"\"; projectRoot = \"\"; targets = (%s);"
        % (proj_cfg_list, root_id, products_group, target))

    def build_cfg(name, settings, base=None):
        i = oid("cfg:" + name + ":" + ("base" if base else "tgt"))
        lines = "".join("\n\t\t\t\t%s = %s;" % (k, v) for k, v in settings)
        b = " baseConfigurationReference = %s;" % base if base else ""
        add(i, name, "isa = XCBuildConfiguration;%s buildSettings = {%s\n\t\t\t}; name = %s;" % (b, lines, name))
        return i

    common_proj = [
        ("ALWAYS_SEARCH_USER_PATHS", "NO"),
        ("CLANG_CXX_LANGUAGE_STANDARD", q("c++17")),
        ("CLANG_ENABLE_OBJC_ARC", "YES"),
        ("CLANG_WARN_BOOL_CONVERSION", "YES"),
        ("CLANG_WARN_EMPTY_BODY", "YES"),
        ("CLANG_WARN_ENUM_CONVERSION", "YES"),
        ("CLANG_WARN_INT_CONVERSION", "YES"),
        ("CLANG_WARN_UNREACHABLE_CODE", "YES"),
        ("ENABLE_USER_SCRIPT_SANDBOXING", "NO"),
        ("GCC_NO_COMMON_BLOCKS", "YES"),
        ("GCC_SYMBOLS_PRIVATE_EXTERN", "YES"),
        ("GCC_WARN_ABOUT_RETURN_TYPE", "YES"),
        ("GCC_WARN_UNUSED_VARIABLE", "YES"),
        ("HEADER_SEARCH_PATHS", '(\n\t\t\t\t\t"$(SRCROOT)/Source",\n\t\t\t\t\t"$(AE_SDK_ROOT)/Headers",\n\t\t\t\t\t"$(AE_SDK_ROOT)/Headers/SP",\n\t\t\t\t\t"$(AE_SDK_ROOT)/Util",\n\t\t\t\t\t"$(AE_SDK_ROOT)/Resources",\n\t\t\t\t)'),
        ("MACOSX_DEPLOYMENT_TARGET", "13.0"),
        ("REZ_PREPROCESSOR_DEFINITIONS", "__MACH__"),
        ("SDKROOT", "macosx"),
        ("WRAPPER_EXTENSION", "plugin"),
    ]
    debug_proj = common_proj + [("GCC_OPTIMIZATION_LEVEL", "1"), ("ONLY_ACTIVE_ARCH", "YES"), ("DEBUG_INFORMATION_FORMAT", "dwarf-with-dsym"), ("GCC_PREPROCESSOR_DEFINITIONS", '("DEBUG=1", "$(inherited)")')]
    release_proj = common_proj + [("GCC_OPTIMIZATION_LEVEL", "3"), ("ONLY_ACTIVE_ARCH", "NO"), ("ARCHS", q("arm64 x86_64")), ("DEBUG_INFORMATION_FORMAT", "dwarf-with-dsym"), ("DEAD_CODE_STRIPPING", "YES"), ("GCC_PREPROCESSOR_DEFINITIONS", '("NDEBUG=1", "$(inherited)")')]

    tgt_settings = [
        ("CODE_SIGN_IDENTITY", q("-")),
        ("CODE_SIGN_STYLE", "Manual"),
        ("COMBINE_HIDPI_IMAGES", "YES"),
        ("GENERATE_PKGINFO_FILE", "YES"),
        ("INFOPLIST_FILE", "Source/Info.plist"),
        ("PRODUCT_BUNDLE_IDENTIFIER", "com.nsdev.AsciiDepth"),
        ("PRODUCT_NAME", PRODUCT),
        ("SKIP_INSTALL", "YES"),
    ]

    pd = build_cfg("Debug", debug_proj, base_cfg)
    pr = build_cfg("Release", release_proj, base_cfg)
    td = build_cfg("Debug", tgt_settings)
    tr = build_cfg("Release", tgt_settings)
    add(proj_cfg_list, 'Build configuration list for PBXProject "%s"' % PRODUCT,
        "isa = XCConfigurationList; buildConfigurations = (\n\t\t\t\t%s /* Debug */,\n\t\t\t\t%s /* Release */,\n\t\t\t); defaultConfigurationIsVisible = 0; defaultConfigurationName = Release;" % (pd, pr))
    add(tgt_cfg_list, 'Build configuration list for PBXNativeTarget "%s"' % PRODUCT,
        "isa = XCConfigurationList; buildConfigurations = (\n\t\t\t\t%s /* Debug */,\n\t\t\t\t%s /* Release */,\n\t\t\t); defaultConfigurationIsVisible = 0; defaultConfigurationName = Release;" % (td, tr))

    # --- write ----------------------------------------------------------------------------------------
    sections = [
        ("PBXBuildFile", "PBXBuildFile"),
        ("PBXFileReference", "PBXFileReference"),
        ("PBXFrameworksBuildPhase", "PBXFrameworksBuildPhase"),
        ("PBXGroup", "PBXGroup"),
        ("PBXNativeTarget", "PBXNativeTarget"),
        ("PBXProject", "PBXProject"),
        ("PBXResourcesBuildPhase", "PBXResourcesBuildPhase"),
        ("PBXShellScriptBuildPhase", "PBXShellScriptBuildPhase"),
        ("PBXSourcesBuildPhase", "PBXSourcesBuildPhase"),
        ("XCBuildConfiguration", "XCBuildConfiguration"),
        ("XCConfigurationList", "XCConfigurationList"),
    ]
    out = ["// !$*UTF8*$!\n{\n\tarchiveVersion = 1;\n\tclasses = {\n\t};\n\tobjectVersion = 56;\n\tobjects = {\n"]
    for isa, _ in sections:
        out.append("\n/* Begin %s section */\n" % isa)
        for i, comment, body in sorted(objects, key=lambda o: o[0]):
            if body.startswith("isa = %s;" % isa):
                out.append("\t\t%s /* %s */ = {%s};\n" % (i, comment.replace("*/", ""), body))
        out.append("/* End %s section */\n" % isa)
    out.append("\t};\n\trootObject = %s /* Project object */;\n}\n" % proj)

    os.makedirs(PROJECT_DIR, exist_ok=True)
    with open(os.path.join(PROJECT_DIR, "project.pbxproj"), "w") as fh:
        fh.write("".join(out))

    # Shared scheme so `xcodebuild -scheme AsciiDepth` works without opening Xcode.
    scheme_dir = os.path.join(PROJECT_DIR, "xcshareddata", "xcschemes")
    os.makedirs(scheme_dir, exist_ok=True)
    with open(os.path.join(scheme_dir, PRODUCT + ".xcscheme"), "w") as fh:
        fh.write(SCHEME.format(target=target, name=PRODUCT))

    print("Wrote %s (%d source files)" % (os.path.relpath(PROJECT_DIR, ROOT), len(source_files)))


SCHEME = """<?xml version="1.0" encoding="UTF-8"?>
<Scheme LastUpgradeVersion = "1620" version = "1.7">
   <BuildAction parallelizeBuildables = "YES" buildImplicitDependencies = "YES">
      <BuildActionEntries>
         <BuildActionEntry buildForTesting = "YES" buildForRunning = "YES" buildForProfiling = "YES" buildForArchiving = "YES" buildForAnalyzing = "YES">
            <BuildableReference BuildableIdentifier = "primary" BlueprintIdentifier = "{target}" BuildableName = "{name}.plugin" BlueprintName = "{name}" ReferencedContainer = "container:{name}.xcodeproj">
            </BuildableReference>
         </BuildActionEntry>
      </BuildActionEntries>
   </BuildAction>
   <TestAction buildConfiguration = "Debug" selectedDebuggerIdentifier = "Xcode.DebuggerFoundation.Debugger.LLDB" selectedLauncherIdentifier = "Xcode.DebuggerFoundation.Launcher.LLDB" shouldUseLaunchSchemeArgsEnv = "YES">
   </TestAction>
   <LaunchAction buildConfiguration = "Debug" selectedDebuggerIdentifier = "Xcode.DebuggerFoundation.Debugger.LLDB" selectedLauncherIdentifier = "Xcode.DebuggerFoundation.Launcher.LLDB" launchStyle = "0" useCustomWorkingDirectory = "NO" ignoresPersistentStateOnLaunch = "NO" debugDocumentVersioning = "YES" debugServiceExtension = "internal" allowLocationSimulation = "YES">
      <PathRunnable runnableDebuggingMode = "0" FilePath = "/Applications/Adobe After Effects 2026/Adobe After Effects 2026.app">
      </PathRunnable>
   </LaunchAction>
   <ProfileAction buildConfiguration = "Release" shouldUseLaunchSchemeArgsEnv = "YES" savedToolIdentifier = "" useCustomWorkingDirectory = "NO" debugDocumentVersioning = "YES">
   </ProfileAction>
   <AnalyzeAction buildConfiguration = "Debug">
   </AnalyzeAction>
   <ArchiveAction buildConfiguration = "Release" revealArchiveInOrganizer = "YES">
   </ArchiveAction>
</Scheme>
"""

if __name__ == "__main__":
    sys.exit(main())
