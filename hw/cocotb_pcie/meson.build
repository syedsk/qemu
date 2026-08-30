cc = meson.get_compiler('c')
zmq = dependency('libzmq', required: false)
if not zmq.found()
  zmq = cc.find_library('zmq', required: true)
endif
system_ss.add(when: 'CONFIG_COCOTB_PCIE', if_true: [
  files('cpd.c', 'cosim_port.c'), zmq,
])

