package local.nvk;

import java.nio.file.Path;
import java.nio.file.Paths;

public final class LaunchContext {
    public static void prepare(String[] arguments, String loader) {
        Path game = Paths.get(System.getProperty("user.dir"));
        boolean client = true;
        for (int i = 0; i < arguments.length; i++) {
            String arg = arguments[i];
            if (arg.equals("--gameDir") && i + 1 < arguments.length) game = Paths.get(arguments[++i]);
            else if (arg.startsWith("--gameDir=")) game = Paths.get(arg.substring("--gameDir=".length()));
            else if (arg.equals("--launchTarget") && i + 1 < arguments.length) {
                client = !arguments[++i].toLowerCase(java.util.Locale.ROOT).contains("server");
            } else if (arg.startsWith("--launchTarget=")) {
                client = !arg.toLowerCase(java.util.Locale.ROOT).contains("server");
            }
        }
        NvkBootstrap.prepare(game, game.resolve("config"), client, loader);
        if (NvkBootstrap.active()) NvkBootstrap.ready();
    }

    public static void prepareLegacy(java.io.File directory) {
        NvkBootstrap.prepare(directory.toPath(), directory.toPath().resolve("config"), true, "forge-launchwrapper");
        if (!NvkBootstrap.active()) return;
        NvkBootstrap.configureLegacySplash();
        try {
            if (Boolean.TRUE.equals(Class.forName("org.lwjgl.opengl.Display").getMethod("isCreated").invoke(null)))
                throw new IllegalStateException("LWJGL 2 already created an OpenGL context before NVK Bootstrap");
            System.load(NvkBootstrap.runtime().resolve("libLwjgl2Zink.dylib").toString());
            NativeEnvironment.installLegacy(Class.forName("org.lwjgl.opengl.GLContext"),
                    Class.forName("org.lwjgl.opengl.MacOSXContextImplementation"));
            NvkBootstrap.ready();
        } catch (ReflectiveOperationException error) { throw new IllegalStateException("Cannot attach LWJGL 2 to Zink", error); }
    }

    public static void configureModLauncherSplash() {
        if (NvkBootstrap.active()) NvkBootstrap.configureModLauncherSplash();
    }

    public static String[] processArguments() {
        try {
            Class<?> process = Class.forName("java.lang.ProcessHandle");
            Object current = process.getMethod("current").invoke(null);
            Object info = process.getMethod("info").invoke(current);
            Object optional = Class.forName("java.lang.ProcessHandle$Info").getMethod("arguments").invoke(info);
            Object value = ((java.util.Optional<?>) optional).orElse(null);
            return value == null ? new String[0] : (String[]) value;
        } catch (ReflectiveOperationException unavailableOnJava8) { return new String[0]; }
    }
}
