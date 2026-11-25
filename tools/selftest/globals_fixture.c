/* Fixture for `make globals-selftest`: tools/globals.sh must flag exactly the
 * mutable symbols below (bss, data, function-local static) and none of the const ones,
 * and the weak ones (a weak definition, a weak reference). */
int fx_bss;
int fx_data = 5;
static int fx_static_bss;
static int fx_static_data = 7;
const int fx_const = 3;
static const char *const fx_tab[] = { "a", "b" };
const char *fx_ptr_tab[] = { "c" }; /* mutable pointer table: flagged */
int fx_fn(void) {
    static int cnt;
    static const int k = 2;
    return ++cnt + k + fx_const + fx_static_bss + fx_static_data + fx_tab[0][0] + fx_ptr_tab[0][0];
}
__attribute__((weak)) int fx_weak_def(void) { return 1; }
extern int fx_weak_ref(void) __attribute__((weak));
int fx_calls_weak(void) { return fx_weak_ref ? fx_weak_ref() : 0; }
