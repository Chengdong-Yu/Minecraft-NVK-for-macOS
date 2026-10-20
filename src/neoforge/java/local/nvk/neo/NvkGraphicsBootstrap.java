package local.nvk.neo;

import local.nvk.LaunchContext;
import net.neoforged.neoforgespi.earlywindow.GraphicsBootstrapper;

public final class NvkGraphicsBootstrap implements GraphicsBootstrapper {
    @Override public String name() { return "nvkbootstrap"; }
    @Override public void bootstrap(String[] arguments) {
        LaunchContext.prepare(arguments, "neoforge");
    }
}
