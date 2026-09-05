# hello_demo.py
# Authored by the NexOS agent INSIDE the Linux compatibility layer.
# Mission: prove the in-guest Python interpreter can WRITE a program
# (hello.py) and then RUN it.  If 'Hello world from NexOS Linux + Python'
# prints, the milestone passes.

# --- write the program ---
f = open('hello.py', 'w')
f.write("print('Hello world from NexOS Linux + Python')\n")
f.close()

# --- show that it was written ---
print('[python] wrote program hello.py:')
print(open('hello.py').read())

# --- run the program we just wrote ---
print('[python] running hello.py ...')
exec(open('hello.py').read())
