module std::os::macos::objc;

$if (env::OS_TYPE == OsType.MACOSX):

define Class = distinct void*;
define Method = distinct void*;
define Ivar = distinct void*;
define Selector = distinct void*;

fault ObjcFailure
{
	CLASS_NOT_FOUND
}

macro char* Class.name(Class cls) = _macos_class_getName(cls);
macro Class Class.superclass(Class cls) = _macos_class_getSuperclass(cls);
macro bool Class.responds_to(Class cls, Selector sel) = _macos_class_respondsToSelector(cls, sel);
macro Method Class.method(Class cls, Selector name) = _macos_class_getClassMethod(cls, name);

macro bool Selector.equals(Selector a, Selector b) = a == b;
macro bool Class.equals(Class a, Class b) = a == b;

macro Selector selector_register(char* c) = _macos_sel_registerName(c);
macro Class! class_by_name(char* c)
{
	Class cls = _macos_objc_lookUpClass(c);
	if (!cls) return ObjcFailure.CLASS_NOT_FOUND!;
	return cls;
}

macro Class[] class_get_list(Allocator *allocator = mem::current_allocator())
{
	int num_classes = _macos_objc_getClassList(null, 0);
	if (!num_classes) return {};
	Class[] entries = array::make(Class, num_classes, allocator);
	_macos_objc_getClassList(entries.ptr, entries.len);
	return entries;
}

extern fn Class _macos_objc_getClass(char* name) @extname("objc_getClass");
extern fn int _macos_objc_getClassList(Class* buffer, int buffer_count) @extname("objc_getClassList");
extern fn char* _macos_class_getName(Class cls) @extname("class_getName");
extern fn Class _macos_class_getSuperclass(Class cls) @extname("class_getSuperclass");
extern fn Method _macos_class_getClassMethod(Class cls, Selector name) @extname("class_getClassMethod");
extern fn bool _macos_class_respondsToSelector(Class cls, Selector name) @extname("class_respondsToSelector");
extern fn Selector _macos_sel_registerName(char* str) @extname("sel_registerName");
extern fn Class _macos_objc_lookUpClass(char* name) @extname("objc_lookUpClass");
$endif;

