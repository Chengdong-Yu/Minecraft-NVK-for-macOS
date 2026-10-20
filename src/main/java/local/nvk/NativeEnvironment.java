package local.nvk;

final class NativeEnvironment {
    static native void installLegacy(Class<?> glContext, Class<?> macContext);
    static native void set(String name, String value);
    static native String checkDriver(String path);
    static native void checkEarlyWindowLibraries();
}
