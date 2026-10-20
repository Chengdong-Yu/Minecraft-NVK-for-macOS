package local.nvk.forge;

import cpw.mods.modlauncher.api.IEnvironment;
import cpw.mods.modlauncher.api.ITransformationService;
import cpw.mods.modlauncher.api.ITransformer;
import java.util.List;
import java.util.Set;
import local.nvk.LaunchContext;

public final class NvkTransformationService implements ITransformationService {
    @Override public String name() { return "nvkbootstrap"; }
    @Override public void initialize(IEnvironment environment) { prepare(environment); }
    @Override public void onLoad(IEnvironment environment, Set<String> services) { prepare(environment); }
    @Override public void beginScanning(IEnvironment environment) { }
    @SuppressWarnings("rawtypes")
    @Override public List<ITransformer> transformers() { return java.util.Collections.emptyList(); }

    private static void prepare(IEnvironment environment) {
        java.util.ArrayList<String> arguments = new java.util.ArrayList<>(java.util.Arrays.asList(LaunchContext.processArguments()));
        environment.getProperty(IEnvironment.Keys.GAMEDIR.get()).ifPresent(path -> {
            arguments.add("--gameDir"); arguments.add(path.toString());
        });
        environment.getProperty(IEnvironment.Keys.LAUNCHTARGET.get()).ifPresent(target -> {
            arguments.add("--launchTarget"); arguments.add(target);
        });
        LaunchContext.prepare(arguments.toArray(new String[0]), "modlauncher");
        try {
            if (ITransformationService.class.getMethod("beginScanning", IEnvironment.class).getReturnType() == Void.TYPE)
                LaunchContext.configureModLauncherSplash();
        } catch (NoSuchMethodException error) { throw new IllegalStateException("Unsupported ModLauncher scanning interface", error); }
    }
}
