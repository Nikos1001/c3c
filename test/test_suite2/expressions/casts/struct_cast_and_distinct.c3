module test;

define Foo = distinct int;

fn void test1()
{
	int[2][*] x = { { 2, 3}, { 5, 6 }};
	Foo[2][2] y = x; // #error: can do an explicit
}

fn void test2()
{
	int[2][*] x = { { 2, 3}, { 5, 6 }};
	Foo[2][2] y = (Foo[2][2])x;
}


fn void test3()
{
	int[2][*] x = { { 2, 3}, { 5, 6 }};
	Foo[2][2]* y = &x; // #error: can do an explicit
}

struct Bar { int x; }

struct Baz { int x; }

fn void test4()
{
	Baz[2][*] x = { { { 2 } , { 3 } }, {{5}, {6} }};
	Bar[*][*] y = (Bar[2][2])x;
}
