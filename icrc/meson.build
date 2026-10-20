project('node-b-rocev2', 'c', version: '0.1.0',
  default_options: ['c_std=c11', 'warning_level=3', 'werror=true'])

inc = include_directories('src')
worker = static_library('node_b_worker', 'src/worker.c', include_directories: inc)
worker_test = executable('test_worker', 'tests/test_worker.c',
  include_directories: inc, link_with: worker, c_args: ['-UNDEBUG'])
test('worker-ownership', worker_test)
python = import('python').find_installation('python3')
test('discovery', python, args: files('tests/test_discovery.py'))

summary('Hardware build enabled', get_option('hardware'))

if get_option('hardware')
  doca_common = dependency('doca-common')
  doca_flow = dependency('doca-flow')
  doca_eth = dependency('doca-eth')
  doca_bridge = dependency('doca-dpdk-bridge')
  dpdk = dependency('libdpdk')
  hardware = static_library('node_b_hardware', ['src/flow.c', 'src/eth_tx.c', 'src/dpdk_rx.c'],
    include_directories: inc,
    c_args: ['-D_GNU_SOURCE', '-DDOCA_ALLOW_EXPERIMENTAL_API'],
    dependencies: [doca_common, doca_flow, doca_eth, doca_bridge, dpdk], link_with: worker)
  executable('node_b', 'src/main.c', include_directories: inc,
    c_args: ['-D_GNU_SOURCE', '-DDOCA_ALLOW_EXPERIMENTAL_API'],
    dependencies: [doca_common, doca_flow, doca_eth, doca_bridge, dpdk], link_with: [hardware, worker])
endif
