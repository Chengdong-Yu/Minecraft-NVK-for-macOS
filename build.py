#!/usr/bin/env python3
from pathlib import Path
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parent
parser = argparse.ArgumentParser()
parser.add_argument('--java-home', type=Path, default=Path.home() / 'Library/Application Support/Swift Craft Launcher/runtime/java-runtime-epsilon/jre.bundle/Contents/Home')
parser.add_argument('--skip-native', action='store_true')
parser.add_argument('--jni-home', type=Path, default=Path.home() / 'Library/Java/JavaVirtualMachines/temurin-21.jdk/Contents/Home')
parser.add_argument('--libraries', type=Path, default=Path.home() / 'Library/Application Support/Swift Craft Launcher/meta/libraries')
args = parser.parse_args()
build = ROOT / 'build'
native = build / 'payload/macos-x86_64'
classes = build / 'classes'
for folder in [native, classes, ROOT / 'dist']:
    folder.mkdir(parents=True, exist_ok=True)

def run(command):
    subprocess.run([str(x) for x in command], check=True)

if not args.skip_native:
    src = ROOT / 'src/main/native'
    common = ['clang', '-dynamiclib', '-O2', '-Wall', '-Wextra', '-Wl,-headerpad_max_install_names']
    run(['swiftc', '-emit-library', '-O', '-swift-version', '5', '-target', 'x86_64-apple-macos11.0', '-module-name', 'NvkCocoa',
         src / 'CocoaBridge.swift', '-o', native / 'libNvkCocoa.dylib',
         '-Xlinker', '-install_name', '-Xlinker', '@rpath/libNvkCocoa.dylib',
         '-Xlinker', '-headerpad_max_install_names'])
    run(common + ['-I' + str(args.jni_home / 'include'), '-I' + str(args.jni_home / 'include/darwin'),
                  src / 'bootstrap.c', '-o', native / 'libNvkBootstrap.dylib'])
    run(common + ['-I' + str(ROOT / 'vendor/Vulkan-Headers/include'), src / 'nvk_native.c', native / 'libNvkCocoa.dylib',
                  '-framework', 'Cocoa', '-framework', 'QuartzCore', '-framework', 'IOSurface',
                  '-framework', 'ImageIO', '-o', native / 'libvulkan.1.dylib'])

    glfw = ROOT / 'vendor/glfw'
    names = ['context.c', 'init.c', 'input.c', 'monitor.c', 'platform.c', 'vulkan.c', 'window.c',
             'egl_context.c', 'osmesa_context.c', 'null_init.c', 'null_monitor.c', 'null_window.c',
             'null_joystick.c', 'cocoa_time.c', 'posix_thread.c', 'posix_module.c', 'cocoa_init.m',
             'cocoa_joystick.m', 'cocoa_monitor.m', 'cocoa_window.m', 'nsgl_context.m']
    egl_override = '-D_GLFW_EGL_LIBRARY=(getenv("MC_EGL_LIBRARY") ? getenv("MC_EGL_LIBRARY") : "@loader_path/libEGL.1.dylib")'
    run(common + ['-D_GLFW_COCOA', egl_override, '-I' + str(glfw / 'include'),
                  '-I' + str(glfw / 'src'), '-I' + str(src)] +
        [glfw / 'src' / name for name in names] + [src / 'glfw_zink_present.c',
                  '-framework', 'Cocoa', '-framework', 'IOKit', '-framework', 'CoreFoundation',
                  '-o', native / 'libglfwZink.dylib'])

    for name in ['libEGL.1.dylib', 'libgallium-26.3.0-devel.dylib', 'libSDL3_original.dylib']:
        shutil.copy2(ROOT / 'vendor/payload' / name, native / name)

    run(['install_name_tool', '-id', '@loader_path/libSDL3_original.dylib', native / 'libSDL3_original.dylib'])
    run(common + ['-I' + str(ROOT / 'vendor/EGL'), src / 'gl_zink.c', native / 'libEGL.1.dylib',
                  '-o', native / 'libGLZink.dylib'])
    run(common + ['-I' + str(args.jni_home / 'include'), '-I' + str(args.jni_home / 'include/darwin'),
                  '-I' + str(ROOT / 'vendor/EGL'), '-I' + str(src), src / 'lwjgl2_zink.c',
                  native / 'libEGL.1.dylib', native / 'libNvkCocoa.dylib', '-framework', 'Cocoa', '-framework', 'QuartzCore',
                  '-o', native / 'libLwjgl2Zink.dylib'])
    run(common + ['-I' + str(ROOT / 'vendor/EGL'), '-I' + str(src), src / 'sdl_zink.c',
                  native / 'libEGL.1.dylib', native / 'libNvkCocoa.dylib', '-Wl,-reexport_library,' + str(native / 'libSDL3_original.dylib'),
                  '-framework', 'Cocoa', '-framework', 'QuartzCore', '-o', native / 'libSDL3NVK.dylib'])

    for library in native.glob('*.dylib'):
        run(['install_name_tool', '-id', '@loader_path/' + library.name, library])
        listing = subprocess.check_output(['otool', '-L', str(library)], text=True)
        for line in listing.splitlines()[2:]:
            dependency = line.strip().split(' (')[0]
            if dependency.startswith('/Users/'):
                sibling = native / Path(dependency).name
                if not sibling.exists():
                    raise RuntimeError('Unbundled dependency: ' + dependency)
                run(['install_name_tool', '-change', dependency, '@loader_path/' + sibling.name, library])
        commands = subprocess.check_output(['otool', '-l', str(library)], text=True).splitlines()
        rpaths = []
        for i, line in enumerate(commands):
            if line.strip() == 'cmd LC_RPATH':
                rpaths.append(commands[i + 2].strip().split(' (offset')[0].removeprefix('path '))
        for path in rpaths:
            if path.startswith('/Users/'):
                run(['install_name_tool', '-delete_rpath', path, library])
        if '@loader_path' not in rpaths:
            run(['install_name_tool', '-add_rpath', '@loader_path', library])
        run(['codesign', '--force', '--sign', '-', '--timestamp=none', library])

entries = []
for p in sorted(native.glob('*.dylib')):
    entries.append((p.name, hashlib.sha256(p.read_bytes()).hexdigest()))
manifest = 'files=' + ','.join(name for name, _ in entries) + '\n'
manifest += ''.join(name + '.sha256=' + digest + '\n' for name, digest in entries)
(native / 'manifest.properties').write_text(manifest)

common_names = ['NvkBootstrap.java', 'NativeEnvironment.java', 'LaunchContext.java']
common_sources = [ROOT / 'src/main/java/local/nvk' / name for name in common_names]
profiles = {
    'fabric': {
        'sources': [p for p in (ROOT / 'src/main/java').rglob('*.java') if p.name not in common_names],
        'dependencies': [args.libraries / 'net/fabricmc/fabric-loader/0.20.0/fabric-loader-0.20.0.jar'],
        'resources': ROOT / 'src/main/resources',
    },
    'forge': {
        'sources': list((ROOT / 'src/forge/java').rglob('*.java')) + list((ROOT / 'src/legacy/java').rglob('*.java')),
        'dependencies': [ROOT / 'vendor/compile/modlauncher-8.1.3.jar', ROOT / 'vendor/compile/forge-legacy-api.jar'],
        'resources': ROOT / 'src/forge/resources',
    },
    'neoforge': {
        'sources': list((ROOT / 'src/neoforge/java').rglob('*.java')) + list((ROOT / 'src/forge/java').rglob('*.java')),
        'dependencies': [ROOT / 'vendor/compile/fml-loader-10.0.36.jar', ROOT / 'vendor/compile/modlauncher-8.1.3.jar'],
        'resources': ROOT / 'src/neoforge/resources',
    },
}
sums = []
for loader, profile in profiles.items():
    destination = build / ('classes-' + loader)
    if destination.exists(): shutil.rmtree(destination)
    destination.mkdir()
    run([args.java_home / 'bin/javac', '--release', '8' if loader == 'forge' else '17', '-proc:none', '-Xlint:all',
         '-cp', os.pathsep.join(str(p) for p in profile['dependencies']), '-d', destination] +
        common_sources + profile['sources'])
    jar = ROOT / ('dist/nvk-bootstrap-0.4.0-' + loader + '.jar')
    with zipfile.ZipFile(jar, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        legacy = 'FMLCorePlugin: local.nvk.legacy.NvkLoadingPlugin\n' if loader == 'forge' else ''
        archive.writestr('META-INF/MANIFEST.MF', 'Manifest-Version: 1.0\nAutomatic-Module-Name: local.nvk.bootstrap\nFMLModType: LIBRARY\nImplementation-Version: 0.4.0\n' + legacy + '\n')
        for base, prefix in [(destination, ''), (profile['resources'], ''),
                             (build / 'payload', 'payload/'), (ROOT / 'notices', 'META-INF/notices/')]:
            for file in sorted(base.rglob('*')):
                if file.is_file(): archive.write(file, prefix + str(file.relative_to(base)))
    digest = hashlib.sha256(jar.read_bytes()).hexdigest()
    sums.append(digest + '  ' + jar.name)
    print('Built', jar, jar.stat().st_size, 'bytes; SHA256', digest)
(ROOT / 'dist/SHA256SUMS').write_text('\n'.join(sums) + '\n')
(build / 'native-sha256.json').write_text(json.dumps(dict(entries), indent=2))
