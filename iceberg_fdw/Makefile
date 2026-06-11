# iceberg_fdw —— Iceberg 只读 FDW（第一阶段骨架）
#
# 编译方式：openGauss 原生 in-tree 机制（参照 contrib/postgres_fdw）。代码物理
# 位于 myidea 仓库，通过在 openGauss 源码树 contrib/ 下建可逆软链接入编译；
# 这样内部内核头（含未导出的 HTAP 头）全部可见，产物 ABI 与 gaussdb 一致。
# 共享库由 contrib-global.mk 的 install-lib 在 make install 时链接生成。
#
# 用法（已 source .gauss_env、PATH 含 binarylibs gcc10.3/gcc/bin）：
#   ln -sfn /sda/myidea/iceberg_fdw <src>/contrib/iceberg_fdw
#   make -C <src>/contrib/iceberg_fdw install top_builddir=<src>
# （软链下需以绝对路径覆盖 top_builddir：make 的物理 CWD 不在源码树内。）
#
# 注：out-of-tree PGXS 暂不可用——安装 include 树未导出内部 HTAP 头
# （access/htap/borrow_mem_pool.h 等），待其补全后可启用 USE_PGXS 分支。

MODULE_big = iceberg_fdw
OBJS       = iceberg_fdw.o option.o

EXTENSION = iceberg_fdw
DATA      = iceberg_fdw--1.0.sql

all: objfiles.txt

# 生成 objfiles.txt 以抑制 gaussdb 顶层重复编译（与 contrib/postgres_fdw 一致）
objfiles.txt: $(OBJS)
	@echo "" > objfiles.txt

objfiles_clean:
	rm -f objfiles.txt

clean: objfiles_clean

subdir = contrib/iceberg_fdw
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk

# extension 共享库不使用 PIE
exclude_option = -fPIE
override CPPFLAGS := $(filter-out $(exclude_option),$(CPPFLAGS))

# in-tree 子系统编译默认不给目标文件加 -fPIC（内核 .o 静态链入 gaussdb），
# 但 extension 须以共享库加载，目标文件需位置无关代码。
override CXXFLAGS += -fPIC
