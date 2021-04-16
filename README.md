# Viper: An Efficient Hybrid PMem-DRAM Key-Value Store
This repository contains the code to our [VLDB '21 paper]().

### Using Viper
Viper is an embedded header-only key-value store for persistent memory.
You can download it and include it in your application. 
Here is a short example of Viper's interface. 

```cpp
#include <iostream>
#include <viper/viper.hpp>

const size_t inital_size = 1073741824;  // 1 GiB
auto viper_db = viper::Viper<uint64_t, uint64_t>::create("/mnt/my/viper/dir", inital_size);

// To modify records in Viper, you need to use a Viper Client.
auto v_client = viper_db->get_client();

for (uint64_t key = 0; key < 10; ++key) {
  const uint64_t value = key + 10;
  v_client.put(key, value);
}

for (uint64_t key = 0; key < 10; ++key) {
  uint64_t value;
  v_client.get(key, &value);
  std::cout << "Record: " << key << " --> " << value << std::endl;
}
```