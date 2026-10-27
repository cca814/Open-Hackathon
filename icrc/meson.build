project('node-b-rocev2', 'c', version: '0.1.0',
  default_options: ['c_std=c11', 'warning_level=3', 'werror=true'])

inc = include_directories('include')
packet = library('node_b_packet', ['src/roce.c', 'src/icrc.c', 'src/groups.c'], include_directories: inc)
worker = static_library('node_b_worker', 'src/worker.c', include_directories: inc, link_with: packet)
worker_test = executable('test_worker', 'tests/test_worker.c',
  include_directories: inc, link_with: worker, c_args: ['-UNDEBUG'])
test('worker-ownership', worker_test)
python = import('python').find_installation('python3')
test('discovery', python, args: files('tests/test_discovery.py'))
test('raw-udp4791-frame', python, args: files('tests/test_raw_udp4791.py'))
test('roce-wire-oracle', python, args: files('tests/test_roce_wire.py'))

summary('Hardware build enabled', get_option('hardware'))

# Some included DOCA 3.5 headers still expose compatibility APIs as deprecated.
# Acknowledge those declarations without disabling other compiler warnings.
if get_option('hardware')
  doca_common = dependency('doca-common')
  doca_flow = dependency('doca-flow')
  doca_eth = dependency('doca-eth')
  doca_bridge = dependency('doca-dpdk-bridge')
  dpdk = dependency('libdpdk')
  hardware = static_library('node_b_hardware', ['src/flow.c', 'src/eth_tx.c', 'src/dpdk_rx.c'],
    include_directories: inc,
    c_args: ['-D_GNU_SOURCE', '-DDOCA_ALLOW_EXPERIMENTAL_API', '-DDOCA_ALLOW_DEPRECATED_API'],
    dependencies: [doca_common, doca_flow, doca_eth, doca_bridge, dpdk], link_with: worker)
  executable('node_b', 'src/main.c', include_directories: inc,
    c_args: ['-D_GNU_SOURCE', '-DDOCA_ALLOW_EXPERIMENTAL_API', '-DDOCA_ALLOW_DEPRECATED_API'],
    dependencies: [doca_common, doca_flow, doca_eth, doca_bridge, dpdk], link_with: [hardware, worker])
endif

groups_check = executable('groups_check', 'tools/groups_check.c', include_directories: inc, link_with: packet)
fanout_test = executable('test_fanout', 'tests/test_fanout.c', include_directories: inc,
  link_with: [worker, packet], c_args: ['-UNDEBUG'])
test('packet-oracle-and-fanout', python,
  args: [files('tests/oracle_packets.py'), packet.full_path(), groups_check.full_path(), fanout_test.full_path()],
  depends: [packet, groups_check, fanout_test], timeout: 120)

bounds_test = executable('test_roce_bounds', 'tests/test_roce_bounds.c',
  include_directories: inc, link_with: packet, c_args: ['-UNDEBUG'])
test('packet-bounds', bounds_test)
