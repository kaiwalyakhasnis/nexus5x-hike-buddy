// LD_PRELOAD shim for vendor daemons run without Android's property service.
// rmt_storage refuses to start unless ro.baseband is "msm" (stock gets it
// from the bootloader's androidboot.baseband=msm via init).
//
// rmt_storage is an Android (bionic) binary, so this library must not pull
// in any other libc: it is built freestanding (-nostdlib), with the few
// string helpers it needs written out below. Android's loader takes it.

#define PROPERTY_VALUE_MAX 92

static const struct { const char* key; const char* value; } kProps[] = {
  {"ro.baseband", "msm"},
};

static int same(const char* a, const char* b) {
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return *a == *b;
}

int property_get(const char* key, char* value, const char* default_value) {
  const char* v = default_value ? default_value : "";
  for (unsigned i = 0; i < sizeof(kProps) / sizeof(kProps[0]); ++i)
    if (same(key, kProps[i].key)) v = kProps[i].value;
  int n = 0;
  while (v[n] && n < PROPERTY_VALUE_MAX - 1) {
    value[n] = v[n];
    n++;
  }
  value[n] = 0;
  return n;
}

int property_set(const char* key, const char* value) {
  (void)key;
  (void)value;
  return 0;
}
