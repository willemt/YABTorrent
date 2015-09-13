# -*- mode: python -*-
# vi: set ft=python :

import sys
import os

def options(opt):
        opt.load('compiler_c')

def configure(conf):
    conf.load('compiler_c')
    conf.load('clib')

    if sys.platform == 'win32':
        conf.check_cc(lib='ws2_32')
        conf.check_cc(lib='psapi')

    conf.check_cc(lib='uv', libpath=[os.getcwd()])

def unit_test(bld, src, ccflag=None, packages=[]):
    target = "build/tests/t_{0}".format(src)

    # collect tests into one area
    bld(rule='sh {0}/deps/cutest/make-tests.sh {0}/tests/{1} > {2}'.format(os.getcwd(), src, target), target=target)

    libs = []

    # build the test program
    bld.program(
        source=[
            "tests/{0}".format(src),
            target,
            ] + bld.clib_c_files("""
                bitfield
                sha1
                cutest
                """.split()),
        target=src[:-2],
        cflags=[
            '-g',
            '-Werror',
        ],
        use='yabbt',
        lib = libs,
        unit_test='yes',
        includes=["./include"] + bld.clib_h_paths("""
                                    bitfield
                                    sha1
                                    cutest
                                    """.split()))
    # run the test
    if sys.platform == 'win32':
        bld(rule='${SRC}',source=src[:-2]+'.exe')
    else:
        bld(rule='export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:. && ./${SRC}', source=src[:-2])

def scenario_test(bld, src, ccflag=None):
    src = "tests/"+src
    target = src+".autogenerated.c"
    bld(rule='sh {0}/deps/cutest/make-tests.sh {0}/{1} > {2}'.format(os.getcwd(), src, target), target=target)

    libs = []
    bld.program(
        source=[
            src,
            src+".autogenerated.c",
            "tests/network_adapter_mock.c",
            "tests/mock_torrent.c",
            "tests/mock_client.c",
            ] + bld.clib_c_files("""
                bipbuffer
                cutest
                sha1
                mt19937ar
                pwp
                """.split()),
        stlibpath=['libuv', '.'],
        target=src[:-2],
        cflags=[
            '-g',
            '-Werror',
            '-Werror=unused-variable',
            '-Werror=uninitialized',
            '-Werror=return-type',
            ],
        lib=libs,
        unit_test='yes',
        includes=[
            "./include",
            "./tests"
            ] + bld.clib_h_paths("""
                bipbuffer
                config-re
                bitfield
                cutest
                sha1
                mt19937ar
                pwp
                """.split()),
        use='yabbt')

    # run the test
    if sys.platform == 'win32':
        bld(rule='${SRC}', source=src[:-2]+'.exe')
    else:
        bld(rule='export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:. && ./${SRC}', source=src[:-2])
        #bld(rule='pwd && export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:. && ./'+src[:-2])


def build(bld):
    bld.load('clib')

    if sys.platform == 'win32':
        platform = '-DWIN32'
    elif sys.platform == 'linux2':
        platform = '-DLINUX'
    else:
        platform = ''

    libyabtorrent_clibs = """
        array-avl-tree
        bag
        bitstream
        chunkybar
        config-re
        event-timer
        heap
        linked-list-hashmap
        pwp
        pseudolru
        sha1
        strndup
        """.split()

    bld.shlib(
        source="""
        src/bt_blacklist.c
        src/bt_choker_leecher.c
        src/bt_choker_seeder.c
        src/bt_blockrw_cache.c
        src/bt_blockrw_mem.c
        src/bt_download_manager.c
        src/bt_peer_manager.c
        src/bt_piece.c
        src/bt_piece_db.c
        src/bt_selector_random.c
        src/bt_selector_rarestfirst.c
        src/bt_selector_sequential.c
        src/bt_util.c
        """.split() + bld.clib_c_files(libyabtorrent_clibs),
        includes=['./include'] + bld.clib_h_paths(libyabtorrent_clibs),
        target='yabbt',
        cflags=[
            '-Werror',
            '-Werror=format',
            '-Werror=int-to-pointer-cast',
            '-g',
            platform,
            '-Werror=unused-variable',
            '-Werror=return-type',
            '-Werror=uninitialized',
            '-Werror=pointer-to-int-cast',
            '-Wcast-align'])

    unit_test(bld, "test_bt.c")
    unit_test(bld, "test_download_manager.c")
    unit_test(bld, "test_peer_manager.c")
    unit_test(bld, 'test_choker_leecher.c')
    unit_test(bld, 'test_choker_seeder.c')
    unit_test(bld, 'test_selector_rarestfirst.c')
    unit_test(bld, 'test_selector_random.c')
    unit_test(bld, 'test_selector_sequential.c')
    unit_test(bld, 'test_piece.c')
    unit_test(bld, 'test_piece_db.c')
    unit_test(bld, 'test_blacklist.c')
    scenario_test(bld, 'test_download_manager_check_pieces.c')
    scenario_test(bld, 'test_scenario_shares_all_pieces.c')
    scenario_test(bld, 'test_scenario_shares_all_pieces_between_each_other.c')
    scenario_test(bld, 'test_scenario_share_20_pieces.c')
    scenario_test(bld, 'test_scenario_three_peers_share_all_pieces_between_each_other.c')

