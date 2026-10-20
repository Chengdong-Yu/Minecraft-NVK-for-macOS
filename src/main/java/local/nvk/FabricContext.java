package local.nvk;

import net.fabricmc.api.EnvType;
import net.fabricmc.loader.api.FabricLoader;

final class FabricContext {
    static void prepare() {
        FabricLoader loader = FabricLoader.getInstance();
        NvkBootstrap.prepare(loader.getGameDir(), loader.getConfigDir(),
                loader.getEnvironmentType() == EnvType.CLIENT, "fabric");
    }
}
