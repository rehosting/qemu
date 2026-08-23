# Penguin-specific TCG plugins.
#
# Kept out of contrib/plugins/ so that rebasing the fork onto upstream QEMU
# never conflicts here.

penguin_plugins = [
  'bootwatch.c',
]

penguin_plugin_targets = []
if get_option('plugins')
  foreach p : penguin_plugins
    penguin_plugin_targets += shared_module(
      fs.stem(p), files(p),
      dependencies: plugins_deps,
      install: true,
      install_dir: get_option('libdir') / 'qemu-plugins',
    )
  endforeach
endif

if penguin_plugin_targets.length() > 0
  alias_target('penguin-plugins', penguin_plugin_targets)
else
  run_target('penguin-plugins', command: [python, '-c', ''])
endif

plugin_modules += penguin_plugin_targets
