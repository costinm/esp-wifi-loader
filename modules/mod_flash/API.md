# `flash` API (44)

The optional signed-upload component. Its public interface is the loader TCP
protocol on port 5227; private host I/O/timer/completion methods are not RPC
methods. It starts its listener at boot when the module manifest requests it.


It publishes cached status at component 44, record 0: `{1: tcp_port, 2: listening,
3: connection_active}`. This is a device_info contribution, not an RPC method.
The host removes it when the module stops. No console is required.
