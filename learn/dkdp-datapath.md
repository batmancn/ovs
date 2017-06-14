使用参考：http://docs.openvswitch.org/en/latest/howto/dpdk/

dpdk模式的bridge(type=netdev)，其datapath在pmd中(也就是收包的线程，这也是合理的)。
具体解释见lib/dpif-netdev.c