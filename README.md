## C++11 Threadpool that supports various styles of tasks.
thread_pool tp(10);

### 1.Functions:

    int foo(int id, int x, int y){
	cout << "thread id: " << id << endl;
        return x+y;
    }

tp.push(foo);

### 2.
