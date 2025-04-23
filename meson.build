project('ssh-tui', 'c',
  version : '0.1.0',
  default_options : ['c_std=c11']
)

ncurses_dep = dependency('ncurses', required : true)

executable('ssh-tui',
  sources: [
    'ssh_tui.c'
  ],
  dependencies : [ncurses_dep],
  install : true
)

executable('scp-tui',
  sources: [
    'scp_tui.c'
  ],
  dependencies : [ncurses_dep],
  install : true
)
