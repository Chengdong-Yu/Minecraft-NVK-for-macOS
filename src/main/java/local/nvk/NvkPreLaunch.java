package local.nvk;

import net.fabricmc.loader.api.entrypoint.PreLaunchEntrypoint;

public final class NvkPreLaunch implements PreLaunchEntrypoint {
    @Override
    public void onPreLaunch() {
        FabricContext.prepare();
        if (!NvkBootstrap.active()) return;
        NvkBootstrap.ready();
    }
}
