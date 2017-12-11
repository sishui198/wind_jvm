/*
 * main.cpp
 *
 *  Created on: 2017年11月2日
 *      Author: zhengxiaolin
 */
#include "wind_jvm.hpp"
#include "runtime/klass.hpp"
#include "native/java_lang_Class.hpp"
#include "native/java_lang_String.hpp"
#include "native/java_lang_Thread.hpp"
#include "native/native.hpp"
#include "system_directory.hpp"
#include "classloader.hpp"
#include "runtime/thread.hpp"
#include <regex>
#include "utils/synchronize_wcout.hpp"

Lock thread_num_lock;
int all_thread_num;

void * scapegoat (void *pp) {
	temp *real = (temp *)pp;
	if (real->cur_thread_obj != nullptr) {
		ThreadTable::add_a_thread(pthread_self(), real->cur_thread_obj);		// the cur_thread_obj is from `java/lang/Thread.start0()`.
	}
	real->thread->start(*real->arg);
	return nullptr;
};

void vm_thread::launch(InstanceOop *cur_thread_obj)
{
	// start one thread
	p.thread = this;
	p.arg = &const_cast<std::list<Oop *> &>(arg);
	p.cur_thread_obj = cur_thread_obj;

	bool inited = wind_jvm::inited();		// 在这里设置一个局部变量并且读取。防止要读取 jvm 下竞态条件的 inited，造成线程不安全。
	pthread_t tid;
	pthread_create(&tid, nullptr, scapegoat, &p);

	if (!inited) {		// if this is the main thread which create the first init --> thread[0], then wait.

		pthread_join(tid, nullptr);

#ifdef DEBUG
		sync_wcout{} << "run over!!!" << std::endl;		// delete
#endif
	}
}

void vm_thread::start(list<Oop *> & arg)
{
	if (wind_jvm::inited() == false) {
		assert(method == nullptr);			// if this is the init thread, method will be nullptr. this thread will get `main()` automatically.
		assert(arg.size() == 0);

		wind_jvm::inited() = true;			// important!
		vm_thread::init_and_do_main();		// init global variables and execute `main()` function.
	} else {
		// if this is not the thread[0], detach itself is okay because no one will pthread_join it.
		pthread_detach(pthread_self());
		assert(this->vm_stack.size() == 0);	// check
		assert(arg.size() == 1);				// run() only has one argument `this`.

		this->vm_stack.push_back(StackFrame(method, nullptr, nullptr, arg));
		this->execute();
	}
}

Oop *vm_thread::execute()
{
	Oop *all_jvm_return_value = nullptr;
	while(!vm_stack.empty()) {		// run over when stack is empty...
		StackFrame & cur_frame = vm_stack.back();
		if (cur_frame.method->is_native()) {
			pc = nullptr;
			// TODO: native.
			std::cerr << "Doesn't support native now." << std::endl;
			assert(false);
		} else {
			auto code = cur_frame.method->get_code();
			// TODO: support Code attributes......
			if (code->code_length == 0) {
				std::cerr << "empty method??" << std::endl;
				assert(false);		// for test. Is empty method valid ??? I dont know...
			}
			pc = code->code;
			Oop * return_val = BytecodeEngine::execute(*this, vm_stack.back(), this->thread_no);
			if (cur_frame.method->is_void()) {		// TODO: in fact, this can be delete. Because It is of no use.
				assert(return_val == nullptr);
				// do nothing
			} else {
				all_jvm_return_value = return_val;
				cur_frame.op_stack.push(return_val);
			}
		}
		vm_stack.pop_back();	// another half push_back() is in wind_jvm() constructor.
	}
	return all_jvm_return_value;
}

Oop * vm_thread::add_frame_and_execute(shared_ptr<Method> new_method, const std::list<Oop *> & list) {
	// for defense:
	int frame_num = this->vm_stack.size();
	uint8_t *backup_pc = this->pc;
	this->vm_stack.push_back(StackFrame(new_method, this->pc, nullptr, list));		// 设置下一帧的 return_pc 是现在的 pc 值，可以用于 printStackTrace。
	Oop * result = BytecodeEngine::execute(*this, this->vm_stack.back(), this->thread_no);
	this->vm_stack.pop_back();
	this->pc = backup_pc;
	assert(frame_num == this->vm_stack.size());
	return result;
}

MirrorOop *vm_thread::get_caller_class_CallerSensitive()
{
	// back-trace. this method is called from: sun_reflect_Reflection.cpp:JVM_GetCallerClass()
	int level = 0;
	int total_levelnum = this->vm_stack.size();
	shared_ptr<Method> m;
	for (list<StackFrame>::reverse_iterator it = this->vm_stack.rbegin(); it != this->vm_stack.rend(); ++it, ++level) {
		m = it->method;
		if (level == 0 || level == 1) {
			// if level == 0, this method must be `getCallerClass`.
			if (level == 0) {
				if (m->get_name() != L"getCallerClass" || m->get_descriptor() != L"()Ljava/lang/Class;")
					assert(false);
			}
			// must be @CallerSensitive
			if (!m->has_annotation_name_in_method(L"Lsun/reflect/CallerSensitive;")) {
				assert(false);
			}
		} else {
			// TODO: openjdk: is_ignored_by_security_stack_walk(), but didn't implement the third switch because I didn't understand it...
			if (m->get_name() == L"invoke:(Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;") {
				continue;	// java/lang/Reflection/Method.invoke(), ignore.
			} else if (m->get_klass()->check_parent(L"sun/reflect/MethodAccessorImpl")) {
				continue;
			} if (m->has_annotation_name_in_method(L"Lsun/reflect/CallerSensitive;")) {	// 自己加的。详见：http://blog.csdn.net/hel_wor/article/details/50199797
				continue;
			} else {
				break;
			}
			// TODO: 第三点名没有明白......有待研究... is_Compiled_lambda_form...
		}
	}
	if (level == total_levelnum) {		// no stackframe any more...
		assert(false);
	}
	m->print_all_attribute_name();		// delete
	return m->get_klass()->get_mirror();
}

void vm_thread::init_and_do_main()
{
	// init.
	{
		java_lang_class::init();		// must init !!!
		auto class_klass = BootStrapClassLoader::get_bootstrap().loadClass(L"java/lang/Class");
		java_lang_class::fixup_mirrors();	// only [basic types] + java.lang.Class + java.lang.Object

		// load String.class
		auto string_klass = std::static_pointer_cast<InstanceKlass>(BootStrapClassLoader::get_bootstrap().loadClass(L"java/lang/String"));


		// 1. create a [half-completed] Thread obj, using the ThreadGroup obj.(for currentThread(), this must be create first!!)
		auto thread_klass = std::static_pointer_cast<InstanceKlass>(BootStrapClassLoader::get_bootstrap().loadClass(L"java/lang/Thread"));
		// TODO: 要不要放到全局？
		InstanceOop *init_thread = thread_klass->new_instance();
		BytecodeEngine::initial_clinit(thread_klass, *this);		// first <clinit>!
		// inject!!
		init_thread->set_field_value(THREAD L":eetop:J", new LongOop((uint64_t)pthread_self()));		// TODO: 这样没有移植性！！要改啊！！！虽然很方便......其实在 linux 下，也是 8 bytes......
		init_thread->set_field_value(THREAD L":priority:I", new IntOop(NormPriority));	// TODO: ......		// runtime/thread.cpp:1026
		// add this Thread obj to ThreadTable!!!	// ......在这里放入的 init_thread 并没有初始化完全。因为它还没有执行构造函数。不过，那也必须放到表中了。因为在 <init> 执行的时候，内部有其他的类要调用 currentThread...... 所以不放入表中不行啊......
		ThreadTable::add_a_thread(pthread_self(), init_thread);


		// 2. create a [System] ThreadGroup obj.
		auto threadgroup_klass = std::static_pointer_cast<InstanceKlass>(BootStrapClassLoader::get_bootstrap().loadClass(L"java/lang/ThreadGroup"));
		// TODO: 要不要放到全局？
		InstanceOop *init_threadgroup = threadgroup_klass->new_instance();
		BytecodeEngine::initial_clinit(threadgroup_klass, *this);		// first <clinit>!
		{	// 注意：这里创建了全局的第一个 System ThreadGroup !!
			// TODO: 放到全局！
			std::list<Oop *> list;
			list.push_back(init_threadgroup);	// $0 = this
			// execute method: java/lang/ThreadGroup.<init>:()V --> private Method!!
			shared_ptr<Method> target_method = threadgroup_klass->get_this_class_method(L"<init>:()V");
			assert(target_method != nullptr);
			this->add_frame_and_execute(target_method, list);
		}
		// 3. INCOMPLETELY create a [Main] ThreadGroup obj.
		InstanceOop *main_threadgroup = threadgroup_klass->new_instance();
		{
			// inject it into `init_thread`!! 否则，届时在 java/lang/SecurityManager <clinit> 时，会自动 getCurrentThread --> get 到 main_threadgroup --> get 到 system_threadgroup. 所以必须先行注入。
			// hack...
			init_thread->set_field_value(THREAD L":group:Ljava/lang/ThreadGroup;", main_threadgroup);
		}
		assert(this->vm_stack.size() == 0);

		// 关于这里，对 Class 的强制执行初始化 + 强行赋值魔改进行说明。
		// 在 java 中，有一个仅仅给 jdk 使用的 Unsafe 类。这个类支持原子的 “compareAndSwap(CAS)” 二合一操作。
		// 在 linux 中也有类似的函数 atom_xchg 这一类，是支持并发的基础设施。
		// 不过在 java 中，这个功能的实现略微繁琐。
		// 因为 java 对地址支持的局限性没法直接操作内存，以及没法直接调用 lock 指令等多种原因，所以仅仅能够通过 jvm 的 native 来执行。
		// Unsafe 类就是支持这个的方法。它通过取一个成员变量相对于类的 offset，传递给 jvm 并且由 jvm 算出目标距离，然后 xchg。
		// 不过这仅仅能够支持 openjdk 类似的 vm。因为他们的 fields 都有固定的偏移 ———— static field 挂在 Klass 外部一个可计算的距离内存中； field 挂载 Oop 外部一个可计算的距离内存中。
		// 可是我的不行......我的无论是 static field，还是 field，统统都经由指针引向内存的某处了。因为 C++ class 的大小是不可变的，而 java field 总大小是完全可变的。
		// 因而，除了像 openjdk 这样挂在外边不占用 class 内存，还能够保持相同的相对距离，我是没有其他的方法了。而且一开始着手写的时候，也根本不知道还会有这种事......竟然还要计算偏移量......
		// 虽然强行计算并且换值肯定也是可以的。不过日后要支持 GC，本来偏移就不定，一个 GC，那刚才求得的 offset 就肯定错了。
		// 严格来说应该算设计问题吧。Unsafe 这里我就不支持了。
		// 而 System 类开启的过程中，一定需要 Unsafe 类来支持 CAS，从而来变更 java/lang/Class 的缓存(cashed) java/lang/reflect/Field。而这个缓存有个开关，就是 uncashed;
		// 这个 uncashed 会在 System 类 initted 之后，可以使用。但是现在我们还没有 init...... System.out 都没做出来呢......
		// 只有魔改了（逃。
		// 所以如你所见，下边我强行初始化了 Class (为了防止改好的值经过初始化，他又变回去了)
		// 所以先初始化然后赋值。以上即经过。
		BytecodeEngine::initial_clinit(std::static_pointer_cast<InstanceKlass>(class_klass), *this);
		std::static_pointer_cast<InstanceKlass>(class_klass)->set_static_field_value(L"useCaches:Z", new IntOop(false));

		// 3.3 load System.class		// 需要在 Main ThreadGroup 之前执行。因为它的初始化会调用 System。因而会自动触发 <clinit> 的。需要提前把 System.class 设为 initialized.
		// 这里要 hack 一下。不执行 System.<clinit> 了，而是手动执行。因为 Java 类库当中的 java/lang/System 这个类，在 <clinit> 中由于 putStatic，
		// 会自动执行 java/lang/Console 的 <clinit>。而这个 <clinit> 会执行 <sun/misc/JavaLangAccess>registerShutdownHook:(IZLjava/lang/Runnable;)V invokeInterface 方法。
		// 但是，真正的引用指向的是 null！！因为这个 sun/misc/JavaLangAccess 引用是经过 java/lang/System::initializeSystemClass() 来设置的......
		// 然而，System 没有执行 <clinit>，是不可能执行方法的......至少在我这里是这样。
		// 虽然看不太明白 openjdk 是怎么搞定这个环节的......主要是因为 openjdk 的 initialize_impl() 方法太长（逃......。不过这个循环依赖的问题也可以依靠 hack 来解决。
		// System <clinit> 中仅仅执行了：java/io/InputStream::<clinit>, java/io/PrintStream::<clinit>, Ljava/lang/SecurityManager::<clinit>。
		// 然后把 System 中的 static 变量：out, in, err 设成 null。也就是初始化打印设备。
		// 然而我这里引用默认是 null。所以不用初始化。因此只要执行 <clinit> 原谅三连就行。
		// 那么就让我们开始吧。仅仅 loadClass 而不 initial_clinit，即仅仅 load class，而不执行 system 的 <clinit>。
		auto system_klass = std::static_pointer_cast<InstanceKlass>(BootStrapClassLoader::get_bootstrap().loadClass(L"java/lang/System"));
		system_klass->set_state(Klass::KlassState::Initializing);
//		BytecodeEngine::initial_clinit(system_klass, *this);
		auto InputStream_klass = std::static_pointer_cast<InstanceKlass>(BootStrapClassLoader::get_bootstrap().loadClass(L"java/io/InputStream"));
		BytecodeEngine::initial_clinit(InputStream_klass, *this);
		auto PrintStream_klass = std::static_pointer_cast<InstanceKlass>(BootStrapClassLoader::get_bootstrap().loadClass(L"java/io/PrintStream"));
		BytecodeEngine::initial_clinit(PrintStream_klass, *this);
		auto SecurityManager_klass = std::static_pointer_cast<InstanceKlass>(BootStrapClassLoader::get_bootstrap().loadClass(L"java/lang/SecurityManager"));
		BytecodeEngine::initial_clinit(SecurityManager_klass, *this);

		// 3.5 COMPLETELY create the [Main] ThreadGroup obj.
		{	// 注意：这里创建了针对此 main 的第二个 System ThreadGroup !!用第一个 System ThreadGroup 作为参数！
			// TODO: pthread_mutex!!
			std::list<Oop *> list;
			list.push_back(main_threadgroup);	// $0 = this
			list.push_back(nullptr);				// $1 = nullptr
			list.push_back(init_threadgroup);	// $2 = init_threadgroup
			list.push_back(java_lang_string::intern(L"main"));	// $3 = L"main"
			// execute method: java/lang/ThreadGroup.<init>:()V --> private Method!!		// 直接调用私有方法！为了避过狗日的 java/lang/SecurityManager 的检查......我也是挺拼的......QAQ
			// TODO: 因为这里是直接调用了私方法，所以有可能是不可移植的。因为它私方法有可能变。
			shared_ptr<Method> target_method = threadgroup_klass->get_this_class_method(L"<init>:(Ljava/lang/Void;Ljava/lang/ThreadGroup;Ljava/lang/String;)V");
			assert(target_method != nullptr);
			this->add_frame_and_execute(target_method, list);
		}

		// 3.7 又需要 hack 一波。因为 java.security.util.Debug 这货需要调用 System 的各种东西，甚至是标准输入输出。因此不能初始化它。要延迟。
		auto Security_DEBUG_klass = std::static_pointer_cast<InstanceKlass>(BootStrapClassLoader::get_bootstrap().loadClass(L"sun/security/util/Debug"));
		Security_DEBUG_klass->set_state(Klass::KlassState::Initializing);

		// 4. [complete] the Thread obj using the [uncomplete] main_threadgroup.
		{
			std::list<Oop *> list;
			list.push_back(init_thread);			// $0 = this
			list.push_back(main_threadgroup);	// $1 = [main_threadGroup]
			list.push_back(java_lang_string::intern(L"main"));	// $2 = L"main"
			// execute method: java/lang/Thread.<init>:(ThreadGroup, String)V --> public Method.
			shared_ptr<Method> target_method = thread_klass->get_this_class_method(L"<init>:(Ljava/lang/ThreadGroup;Ljava/lang/String;)V");
			assert(target_method != nullptr);
			this->add_frame_and_execute(target_method, list);
		}

		// 3.3 Complete! invoke the method...
		// java/lang/System::initializeSystemClass(): "Initialize the system class.  Called after thread initialization." So it must be created after the thread.
		shared_ptr<Method> _initialize_system_class = system_klass->get_this_class_method(L"initializeSystemClass:()V");
		this->add_frame_and_execute(_initialize_system_class, {});
		system_klass->set_state(Klass::KlassState::Initialized);		// set state.

		// 3.7 Complete!
		BytecodeEngine::initial_clinit(SecurityManager_klass, *this);
		Security_DEBUG_klass->set_state(Klass::KlassState::Initialized);

	}

	auto Perf_klass = std::static_pointer_cast<InstanceKlass>(BootStrapClassLoader::get_bootstrap().loadClass(L"sun/misc/Perf"));
	Perf_klass->set_state(Klass::KlassState::Initializing);				// 禁用 Perf.
	auto PerfCounter_klass = std::static_pointer_cast<InstanceKlass>(BootStrapClassLoader::get_bootstrap().loadClass(L"sun/misc/PerfCounter"));
	PerfCounter_klass->set_state(Klass::KlassState::Initializing);		// 禁用 PerfCounter.

	auto launcher_helper_klass = std::static_pointer_cast<InstanceKlass>(BootStrapClassLoader::get_bootstrap().loadClass(L"sun/launcher/LauncherHelper"));
	BytecodeEngine::initial_clinit(launcher_helper_klass, *this);
	shared_ptr<Method> load_main_method = launcher_helper_klass->get_this_class_method(L"checkAndLoadMain:(ZILjava/lang/String;)Ljava/lang/Class;");
	// new a String.
	InstanceOop *main_klass = (InstanceOop *)java_lang_string::intern(wind_jvm::main_class_name());

	this->vm_stack.push_back(StackFrame(load_main_method, nullptr, nullptr, {new IntOop(true), new IntOop(1), main_klass}));		// TODO: 暂时设置 main 方法的 return_pc 和 prev 全是 nullptr。
	MirrorOop *main_class_mirror = (MirrorOop *)this->execute();
	assert(main_class_mirror->get_ooptype() == OopType::_InstanceOop);

	// first execute <clinit> if has
	BytecodeEngine::initial_clinit(std::static_pointer_cast<InstanceKlass>(main_class_mirror->get_mirrored_who()), *this);
	// get the `public static void main()`.
	auto main_method = std::static_pointer_cast<InstanceKlass>(main_class_mirror->get_mirrored_who())->get_static_void_main();
	// new a String[], for the arguments.
	ObjArrayOop *string_arr_oop = (ObjArrayOop *)std::static_pointer_cast<ObjArrayKlass>(BootStrapClassLoader::get_bootstrap().loadClass(L"[Ljava/lang/String;"))->new_instance(wind_jvm::argv().size());
	auto iter = system_classmap.find(L"java/lang/String.class");
	assert(iter != system_classmap.end());
	auto string_klass = std::static_pointer_cast<InstanceKlass>((*iter).second);
	for (int i = 0; i < wind_jvm::argv().size(); i ++) {
		(*string_arr_oop)[i] = java_lang_string::intern(wind_jvm::argv()[i]);
	}

	// load some useful klass...
	{
		auto methodtype_klass = std::static_pointer_cast<InstanceKlass>(BootStrapClassLoader::get_bootstrap().loadClass(L"java/lang/invoke/MethodType"));
		BytecodeEngine::initial_clinit(methodtype_klass, *this);
		auto methodhandle_klass = std::static_pointer_cast<InstanceKlass>(BootStrapClassLoader::get_bootstrap().loadClass(L"java/lang/invoke/MethodHandle"));
		BytecodeEngine::initial_clinit(methodhandle_klass, *this);

	}




	// The World's End!
	this->vm_stack.push_back(StackFrame(main_method, nullptr, nullptr, {string_arr_oop}));		// TODO: 暂时设置 main 方法的 return_pc 和 prev 全是 nullptr。
	this->execute();

	// kill all other running thread...
	ThreadTable::kill_all_except_main_thread(pthread_self());

//	auto klass = std::static_pointer_cast<InstanceKlass>(BootStrapClassLoader::get_bootstrap().loadClass(L"sun/misc/Launcher$AppClassLoader"));
//
//	// TODO: 不应该用 MyClassLoader ！！ 应该用 Java 写的 AppClassLoader!!!
//	shared_ptr<Klass> main_class = MyClassLoader::get_loader().loadClass(wind_jvm::main_class_name());		// this time, "java.lang.Object" has been convert to "java/lang/Object".
//	shared_ptr<Method> main_method = std::static_pointer_cast<InstanceKlass>(main_class)->get_static_void_main();
//	assert(main_method != nullptr);
//	// TODO: 方法区，多线程，堆区，垃圾回收！现在的目标只是 BytecodeExecuteEngine，将来要都加上！！
//
//	// second execute [public static void main].
//

}

ArrayOop * vm_thread::get_stack_trace()
{
	auto stack_elem_arr_klass = std::static_pointer_cast<ObjArrayKlass>(BootStrapClassLoader::get_bootstrap().loadClass(L"[Ljava/lang/StackTraceElement;"));
	auto stack_elem_klass = std::static_pointer_cast<InstanceKlass>(BootStrapClassLoader::get_bootstrap().loadClass(L"java/lang/StackTraceElement"));
	auto stack_elem_init = stack_elem_klass->get_this_class_method(L"<init>:(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;I)V");
	assert(stack_elem_init != nullptr);

	auto arr = (ArrayOop *)stack_elem_arr_klass->new_instance(this->vm_stack.size());

	std::wstringstream ss;

	uint8_t *last_frame_pc = this->pc;		// 先设置成 pc。下边会修改。
	int i = 0;
	for (list<StackFrame>::reverse_iterator it = this->vm_stack.rbegin(); it != this->vm_stack.rend(); ++it) {
		shared_ptr<Method> m = it->method;
		auto klass_name = java_lang_string::intern(m->get_klass()->get_name());
		auto method_name = java_lang_string::intern(m->get_name());
		auto file_name = java_lang_string::intern(m->get_klass()->get_source_file_name());
		int line_num;
		int last_pc_debug = 0;
		if (last_frame_pc == 0)
			line_num = 0;
		else {
			last_pc_debug = last_frame_pc - m->get_code()->code;
			line_num = m->get_java_source_lineno(last_frame_pc - m->get_code()->code);
		}

		// set next frame's pc
		last_frame_pc = it->return_pc;

		// inject
		(*arr)[i] = stack_elem_klass->new_instance();
		((InstanceOop *)(*arr)[i])->set_field_value(STACKTRACEELEMENT L":declaringClass:" STR, klass_name);
		((InstanceOop *)(*arr)[i])->set_field_value(STACKTRACEELEMENT L":methodName:" STR,     method_name);
		((InstanceOop *)(*arr)[i])->set_field_value(STACKTRACEELEMENT L":fileName:" STR,       file_name);
		((InstanceOop *)(*arr)[i])->set_field_value(STACKTRACEELEMENT L":lineNumber:I",        new IntOop(line_num));

//#ifdef DEBUG
	ss << "[backtrace " << this->vm_stack.size() - i - 1 << "] pc: [" << last_pc_debug << "], at <" << m->get_klass()->get_name() << ">::[" << m->get_name() << "], at [" << m->get_klass()->get_source_file_name() << "], line [" << line_num << "]." << std::endl;
	int j = 1;
	for (Oop * value : it->localVariableTable) {
		if (value == nullptr) {
			ss << "    the localVariableTable[" << j-1 << "] is [null]" << std::endl;
		} else if (value->get_ooptype() == OopType::_BasicTypeOop) {
			switch(((BasicTypeOop *)value)->get_type()) {
				case Type::BOOLEAN:
					ss << "    the localVariableTable[" << j-1 << "] is [Z]: [" << ((IntOop *)value)->value << "]" << std::endl;
					break;
				case Type::BYTE:
					ss << "    the localVariableTable[" << j-1 << "] is [B]: [" << ((IntOop *)value)->value << "]" << std::endl;
					break;
				case Type::SHORT:
					ss << "    the localVariableTable[" << j-1 << "] is [S]: [" << ((IntOop *)value)->value << "]" << std::endl;
					break;
				case Type::INT:
					ss << "    the localVariableTable[" << j-1 << "] is [I]: [" << ((IntOop *)value)->value << "]" << std::endl;
					break;
				case Type::CHAR:
					ss << "    the localVariableTable[" << j-1 << "] is [C]: ['" << (wchar_t)((IntOop *)value)->value << "']" << std::endl;
					break;
				case Type::FLOAT:
					ss << "    the localVariableTable[" << j-1 << "] is [F]: ['" << ((FloatOop *)value)->value << "']" << std::endl;
					break;
				case Type::LONG:
					ss << "    the localVariableTable[" << j-1 << "] is [L]: ['" << ((LongOop *)value)->value << "']" << std::endl;
					break;
				case Type::DOUBLE:
					ss << "    the localVariableTable[" << j-1 << "] is [D]: ['" << ((DoubleOop *)value)->value << "']" << std::endl;
					break;
				default:
					assert(false);
			}
		} else if (value->get_ooptype() == OopType::_TypeArrayOop) {
			ss << "    the localVariableTable[" << j-1 << "] is TypeArrayOop." << std::endl;
		} else if (value->get_ooptype() == OopType::_ObjArrayOop) {
			ss << "    the localVariableTable[" << j-1 << "] is ObjArrayOop." << std::endl;
		} else {		// InstanceOop
			if (value != nullptr && value->get_klass() != nullptr && value->get_klass()->get_name() == L"java/lang/String") {		// 特例：如果是 String，就打出来～
				ss << "    the localVariableTable[" << j-1 << "] is java/lang/String: [\"" << java_lang_string::stringOop_to_wstring((InstanceOop *)value) << "\"]" << std::endl;
			} else if (value != nullptr && value->get_klass() != nullptr && value->get_klass()->get_name() == L"java/lang/Class") {			// 特例：如果是 Class，就打出来～
				wstring type = ((MirrorOop *)value)->get_mirrored_who() == nullptr ? ((MirrorOop *)value)->get_extra() : ((MirrorOop *)value)->get_mirrored_who()->get_name();
				ss << "    the localVariableTable[" << j-1 << "] is java/lang/Class: [\"" << type << "\"]" << std::endl;
			} else {
				auto real_klass = std::static_pointer_cast<InstanceKlass>(value->get_klass());
//				auto toString = real_klass->get_this_class_method(L"toString:()Ljava/lang/String;");
//				assert(toString != nullptr);
//				ss << "    the "<< j-1 << " argument is java/lang/Class: [\"" << type << "\"]" << std::endl;
//				this->add_frame_and_execute(toString, {value});	// 会直接输出到控制台...因此算了...
				ss << "    the localVariableTable[" << j-1 << "] is " << real_klass->get_name() << ": [unknown value]" << std::endl;
			}
		}
		j ++;
	}
//#endif

		i ++;
	}

//#ifdef DEBUG
	sync_wcout{} << "===-------------------------------------- printStackTrace() -----------------------------------------===" << std::endl;
	sync_wcout{} << ss.str();
	sync_wcout{} << "===--------------------------------------------------------------------------------------------------===" << std::endl;
//#endif

	return arr;
}

void wind_jvm::run(const wstring & main_class_name, const vector<wstring> & argv)
{
	wind_jvm::main_class_name() = std::regex_replace(main_class_name, std::wregex(L"\\."), L"/");
	wind_jvm::argv() = const_cast<vector<wstring> &>(argv);

	vm_thread *init_thread;
	wind_jvm::lock().lock();
	{
		wind_jvm::threads().push_back(vm_thread(nullptr, {}));
		init_thread = &wind_jvm::threads().back();
	}
	wind_jvm::lock().unlock();

	// 在这里，需要初始化全局变量。线程还没有开启。
	init_native();

	init_thread->launch();		// begin this thread.
}
