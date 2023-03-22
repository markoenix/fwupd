if gusb.found()
cargs = ['-DG_LOG_DOMAIN="FuPluginJabraGnp"']

plugin_quirks += files('jabra_gnp.quirk')
plugin_builtins += static_library('fu_plugin_jabra_gnp',
  sources: [
    'fu-jabra-gnp-plugin.c',
    'fu-jabra-gnp-device.c',
  ],
  include_directories: plugin_incdirs,
  link_with: plugin_libs,
  c_args: cargs,
  dependencies: plugin_deps,
)
endif
