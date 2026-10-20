package local.nvk;

import java.io.IOException;
import java.io.InputStream;
import java.nio.channels.FileChannel;
import java.nio.channels.FileLock;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.nio.file.StandardOpenOption;
import java.security.MessageDigest;
import java.io.ByteArrayOutputStream;
import java.nio.file.Paths;
import java.util.Locale;
import java.util.Properties;

final class NvkBootstrap {
    private static final String PAYLOAD = "/payload/macos-x86_64/";
    private static final String DRIVER = "/Library/GPUBundles/nvmtl/libvulkan_nouveau.dylib";
    private static boolean prepared;
    private static boolean active;
    private static String loader;
    private static Path runtime;
    private static Path game;

    static synchronized void prepare(Path gameDirectory, Path configDirectory, boolean client, String loaderName) {
        if (prepared) return;
        prepared = true;
        try {
            game = gameDirectory.toAbsolutePath().normalize();
            loader = loaderName;
            if (!client) return;
            if (!System.getProperty("os.name").toLowerCase(Locale.ROOT).contains("mac")) {
                System.err.println("[NVK-BOOTSTRAP] Inactive: this build supports macOS x86_64 only");
                return;
            }
            String arch = System.getProperty("os.arch");
            if (!arch.equals("x86_64") && !arch.equals("amd64")) {
                System.err.println("[NVK-BOOTSTRAP] Inactive: unsupported architecture " + arch);
                return;
            }
            Path config = configDirectory.resolve("nvk-bootstrap.properties");
            Properties settings = settings(config);
            if (!Boolean.parseBoolean(settings.getProperty("enabled", "true"))) {
                System.err.println("[NVK-BOOTSTRAP] Disabled by " + config);
                return;
            }
            if (!settings.getProperty("mode", "auto").equals("auto")) {
                System.err.println("[NVK-BOOTSTRAP] Legacy mode setting ignored; the actual graphics API selects the route");
            }
            if (!Files.isRegularFile(Paths.get(DRIVER))) {
                throw new IllegalStateException("NullMoth NVIDIA/NVK driver is required: " + DRIVER);
            }
            runtime = extract();
            System.load(runtime.resolve("libNvkBootstrap.dylib").toString());
            NativeEnvironment.checkEarlyWindowLibraries();
            String driver = NativeEnvironment.checkDriver(DRIVER);
            String presenter = runtime.resolve("libvulkan.1.dylib").toString();
            NativeEnvironment.set("MESA_SHADER_CACHE_DIR", game.resolve(".nvk-bootstrap/shader-cache").toString());
            NativeEnvironment.set("MC_GRAPHICS_AUDIT", Boolean.parseBoolean(settings.getProperty("audit", "false")) ? "1" : "0");
            NativeEnvironment.set("MC_NVK_LIBRARY", presenter);
            System.setProperty("org.lwjgl.vulkan.libname", presenter);
            NativeEnvironment.set("LIBGL_ALWAYS_SOFTWARE", null);
            NativeEnvironment.set("MESA_VK_DEVICE_SELECT", null);
            NativeEnvironment.set("ZINK_NO_KOPPER", null);
            NativeEnvironment.set("MC_ZINK_EGL", "1");
            NativeEnvironment.set("MESA_LOADER_DRIVER_OVERRIDE", "zink");
            NativeEnvironment.set("GALLIUM_DRIVER", "zink");
            NativeEnvironment.set("MC_EGL_LIBRARY", runtime.resolve("libEGL.1.dylib").toString());
            NativeEnvironment.set("MC_SDL_ORIGINAL", runtime.resolve("libSDL3_original.dylib").toString());
            NativeEnvironment.set("MC_ROUTE_STATUS_FILE", game.resolve(".nvk-bootstrap/renderer-route.properties").toString());
            NativeEnvironment.set("SDL_VULKAN_LIBRARY", presenter);
            NativeEnvironment.set("VK_ICD_FILENAMES", "/Library/GPUBundles/nvmtl/nvk_icd.json");
            System.setProperty("org.lwjgl.opengl.explicitInit", "false");
            System.setProperty("org.lwjgl.opengl.libname", runtime.resolve("libGLZink.dylib").toString());
            System.setProperty("org.lwjgl.glfw.libname", runtime.resolve("libglfwZink.dylib").toString());
            System.setProperty("org.lwjgl.sdl.libname", runtime.resolve("libSDL3NVK.dylib").toString());
            active = true;
            System.err.println("[NVK-BOOTSTRAP] Armed OpenGL/Zink and Vulkan/NVK routes; loader=" + loader + "; runtime=" + runtime + "; driver=" + driver);
        } catch (Exception | LinkageError error) {
            throw new IllegalStateException("NVK Bootstrap could not initialize. See the cause below; no fallback renderer was selected.", error);
        }
    }

    private static byte[] read(InputStream in) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] buffer = new byte[16384];
        for (int n; (n = in.read(buffer)) != -1;) out.write(buffer, 0, n);
        return out.toByteArray();
    }

    private static Properties settings(Path config) throws IOException {
        Properties properties = new Properties();
        if (Files.exists(config)) {
            try (InputStream in = Files.newInputStream(config)) { properties.load(in); }
        } else {
            Files.createDirectories(config.getParent());
            Files.write(config, ( "# NVK Bootstrap: macOS x86_64 with NullMoth NVIDIA driver\n" +
                    "enabled=true\naudit=false\n").getBytes(StandardCharsets.UTF_8), StandardOpenOption.CREATE_NEW);
        }
        return properties;
    }

    private static Path extract() throws Exception {
        byte[] manifestBytes;
        try (InputStream in = resource("manifest.properties")) { manifestBytes = read(in); }
        Properties manifest = new Properties();
        manifest.load(new java.io.ByteArrayInputStream(manifestBytes));
        String tag = hash(manifestBytes).substring(0, 16);
        Path directory = game.resolve(".nvk-bootstrap/macos-x86_64-" + tag);
        Files.createDirectories(directory);
        try (FileChannel channel = FileChannel.open(directory.resolve("extract.lock"),
                StandardOpenOption.CREATE, StandardOpenOption.WRITE); FileLock ignored = channel.lock()) {
            for (String name : manifest.getProperty("files").split(",")) {
                if (!name.matches("[A-Za-z0-9_.-]+")) throw new IOException("Invalid payload name: " + name);
                Path target = directory.resolve(name);
                String expected = manifest.getProperty(name + ".sha256");
                if (expected == null) throw new IOException("Missing payload hash: " + name);
                if (Files.isRegularFile(target) && hash(Files.readAllBytes(target)).equals(expected)) continue;
                byte[] data;
                try (InputStream in = resource(name)) { data = read(in); }
                if (!hash(data).equals(expected)) throw new IOException("Payload hash mismatch: " + name);
                Path temp = Files.createTempFile(directory, name, ".part");
                try {
                    Files.write(temp, data);
                    Files.move(temp, target, StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
                } finally { Files.deleteIfExists(temp); }
            }
        }
        return directory;
    }

    private static InputStream resource(String name) throws IOException {
        InputStream in = NvkBootstrap.class.getResourceAsStream(PAYLOAD + name);
        if (in == null) throw new IOException("Missing native payload: " + name);
        return in;
    }

    private static String hash(byte[] bytes) throws Exception {
        StringBuilder text = new StringBuilder();
        for (byte b : MessageDigest.getInstance("SHA-256").digest(bytes)) text.append(String.format("%02x", b & 255));
        return text.toString();
    }

    static boolean active() { return active; }
    static Path runtime() { return runtime; }

    static void configureLegacySplash() {
        Path path = game.resolve("config/splash.properties");
        try {
            byte[] before = Files.exists(path) ? Files.readAllBytes(path) : new byte[0];
            Properties splash = new Properties();
            splash.load(new java.io.ByteArrayInputStream(before));
            if (!Boolean.parseBoolean(splash.getProperty("enabled", "true"))) return;
            Path backup = game.resolve(".nvk-bootstrap/backups/splash-" + hash(before).substring(0, 12) + ".properties");
            Files.createDirectories(backup.getParent());
            if (!Files.exists(backup)) Files.write(backup, before);
            splash.setProperty("enabled", "false");
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            splash.store(out, "NVK: legacy Forge's CGL splash context is incompatible with EGL/Zink");
            Files.write(path, out.toByteArray());
            System.err.println("[NVK-BOOTSTRAP] Disabled legacy Forge CGL splash; original saved to " + backup);
        } catch (Exception error) { throw new IllegalStateException("Cannot configure the legacy Forge splash", error); }
    }

    static void configureModLauncherSplash() {
        Path path = game.resolve("config/fml.toml");
        try {
            byte[] before = Files.exists(path) ? Files.readAllBytes(path) : new byte[0];
            String text = new String(before, StandardCharsets.UTF_8);
            String after = java.util.regex.Pattern.compile("(?m)^splashscreen\\s*=").matcher(text).find()
                    ? text.replaceAll("(?m)^splashscreen\\s*=.*$", "splashscreen = false")
                    : "splashscreen = false\n" + text;
            if (after.equals(text)) return;
            Path backup = game.resolve(".nvk-bootstrap/backups/fml-" + hash(before).substring(0, 12) + ".toml");
            Files.createDirectories(backup.getParent());
            if (!Files.exists(backup)) Files.write(backup, before);
            Files.createDirectories(path.getParent());
            Files.write(path, after.getBytes(StandardCharsets.UTF_8));
            System.err.println("[NVK-BOOTSTRAP] Disabled legacy ModLauncher splash; original saved to " + backup);
        } catch (Exception error) { throw new IllegalStateException("Cannot configure the ModLauncher splash", error); }
    }

    static void ready() {
        System.err.println("[NVK-BOOTSTRAP] Ready: actual OpenGL/Vulkan calls select the route; loader=" + loader);
        try {
            Files.write(game.resolve(".nvk-bootstrap/last-start.properties"), (
                    "routing=actual-api\nloader=" + loader + "\nruntime=" + runtime + "\ndriver=" + DRIVER + "\nphase=preLaunch-ready\n").getBytes(StandardCharsets.UTF_8));
        } catch (IOException e) { System.err.println("[NVK-BOOTSTRAP] Could not write startup status: " + e); }
    }
}
