module test;

fn void test()
{
    int[3]! x;
    int g;
    foreach_r (z : x) // #error: The expression may not be failable.
    {
        g += z;
        x[0] = 1;
    }
}
