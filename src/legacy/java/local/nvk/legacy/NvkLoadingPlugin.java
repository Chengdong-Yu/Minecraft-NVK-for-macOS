package local.nvk.legacy;

import java.io.File;
import java.util.Map;
import local.nvk.LaunchContext;
import net.minecraftforge.fml.relauncher.IFMLLoadingPlugin;

public final class NvkLoadingPlugin implements IFMLLoadingPlugin {
    public NvkLoadingPlugin() {
        if (net.minecraftforge.fml.relauncher.FMLLaunchHandler.side() != net.minecraftforge.fml.relauncher.Side.CLIENT) return;
        Object[] data = net.minecraftforge.fml.relauncher.FMLInjectionData.data();
        if (!(data[6] instanceof File)) throw new IllegalStateException("Forge did not provide its game directory");
        LaunchContext.prepareLegacy((File) data[6]);
    }
    @Override public String[] getASMTransformerClass() { return new String[0]; }
    @Override public String getModContainerClass() { return null; }
    @Override public String getSetupClass() { return null; }
    @Override public void injectData(Map<String, Object> data) { }
    @Override public String getAccessTransformerClass() { return null; }
}
