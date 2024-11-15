#include "SYCLStream.h"

#include <iostream>

using namespace cl::sycl;


// Cache list of devices
bool cached = false;
std::vector<device> devices;
void getDeviceList(void);
program * p;

template <class T>
SYCLStream<T>::SYCLStream(const unsigned int ARRAY_SIZE, const int device_index)
{
  if (!cached)
    getDeviceList();

  array_size = ARRAY_SIZE;

  if (device_index >= devices.size())
    throw std::runtime_error("Invalid device index");
  device dev = devices[device_index];

  if (dev.is_cpu())
  {
    dot_num_groups = dev.get_info<info::device::max_compute_units>();
    dot_wgsize     = dev.get_info<info::device::native_vector_width_double>() * 2;
  }
  else
  {
    dot_num_groups = dev.get_info<info::device::max_compute_units>() * 4;
    dot_wgsize     = dev.get_info<info::device::max_work_group_size>();
  }

  std::cout << "Using SYCL device " << getDeviceName(device_index) << std::endl;
  std::cout << "Driver: " << getDeviceDriver(device_index) << std::endl;
  std::cout << "Reduction kernel config: " << dot_num_groups << " groups of size " << dot_wgsize << std::endl;

  queue = new cl::sycl::queue(dev);

  p = new program(queue->get_context());
  p->build_from_kernel_name<init_kernel>();
  p->build_from_kernel_name<copy_kernel>();
  p->build_from_kernel_name<mul_kernel>();
  p->build_from_kernel_name<add_kernel>();
  p->build_from_kernel_name<triad_kernel>();
  p->build_from_kernel_name<dot_kernel>();

  d_a = new buffer<T>(array_size);
  d_b = new buffer<T>(array_size);
  d_c = new buffer<T>(array_size);
  d_sum = new buffer<T>(dot_num_groups);
}

template <class T>
SYCLStream<T>::~SYCLStream()
{
  delete d_a;
  delete d_b;
  delete d_c;
  delete d_sum;

  delete p;
  delete queue;
}

template <class T>
void SYCLStream<T>::copy()
{
  queue->submit([&](handler &cgh)
  {
    auto ka = d_a->template get_access<access::mode::read>(cgh);
    auto kc = d_c->template get_access<access::mode::write>(cgh);
    cgh.parallel_for<copy_kernel>(p->get_kernel<copy_kernel>(),
          range<1>{array_size}, [=](item<1> item)
    {
      auto id = item.get()[0];
      kc[id] = ka[id];
    });
  });
  queue->wait();
}

template <class T>
void SYCLStream<T>::mul()
{
  const T scalar = startScalar;
  queue->submit([&](handler &cgh)
  {
    auto kb = d_b->template get_access<access::mode::write>(cgh);
    auto kc = d_c->template get_access<access::mode::read>(cgh);
    cgh.parallel_for<mul_kernel>(p->get_kernel<mul_kernel>(),
      range<1>{array_size}, [=](item<1> item)
    {
      auto id = item.get()[0];
      kb[id] = scalar * kc[id];
    });
  });
  queue->wait();
}

template <class T>
void SYCLStream<T>::add()
{
  queue->submit([&](handler &cgh)
  {
    auto ka = d_a->template get_access<access::mode::read>(cgh);
    auto kb = d_b->template get_access<access::mode::read>(cgh);
    auto kc = d_c->template get_access<access::mode::write>(cgh);
    cgh.parallel_for<add_kernel>(p->get_kernel<add_kernel>(),
      range<1>{array_size}, [=](item<1> item)
    {
      auto id = item.get()[0];
      kc[id] = ka[id] + kb[id];
    });
  });
  queue->wait();
}

template <class T>
void SYCLStream<T>::triad()
{
  const T scalar = startScalar;
  queue->submit([&](handler &cgh)
  {
    auto ka = d_a->template get_access<access::mode::write>(cgh);
    auto kb = d_b->template get_access<access::mode::read>(cgh);
    auto kc = d_c->template get_access<access::mode::read>(cgh);
    cgh.parallel_for<triad_kernel>(p->get_kernel<triad_kernel>(),
      range<1>{array_size}, [=](item<1> item)
    {
      auto id = item.get()[0];
      ka[id] = kb[id] + scalar * kc[id];
    });
  });
  queue->wait();
}

template <class T>
T SYCLStream<T>::dot()
{
  queue->submit([&](handler &cgh)
  {
    auto ka   = d_a->template get_access<access::mode::read>(cgh);
    auto kb   = d_b->template get_access<access::mode::read>(cgh);
    auto ksum = d_sum->template get_access<access::mode::write>(cgh);

    auto wg_sum = accessor<T, 1, access::mode::read_write, access::target::local>(range<1>(dot_wgsize), cgh);

    size_t N = array_size;

    cgh.parallel_for<dot_kernel>(p->get_kernel<dot_kernel>(),
      nd_range<1>(dot_num_groups*dot_wgsize, dot_wgsize), [=](nd_item<1> item)
    {
      size_t i = item.get_global(0);
      size_t li = item.get_local(0);
      size_t global_size = item.get_global_range()[0];

      wg_sum[li] = 0.0;
      for (; i < N; i += global_size)
        wg_sum[li] += ka[i] * kb[i];

      size_t local_size = item.get_local_range()[0];
      for (int offset = local_size / 2; offset > 0; offset /= 2)
      {
        item.barrier(cl::sycl::access::fence_space::local_space);
        if (li < offset)
          wg_sum[li] += wg_sum[li + offset];
      }

      if (li == 0)
        ksum[item.get_group(0)] = wg_sum[0];
    });
  });

  T sum = 0.0;
  auto h_sum = d_sum->template get_access<access::mode::read, access::target::host_buffer>();
  for (int i = 0; i < dot_num_groups; i++)
  {
    sum += h_sum[i];
  }

  return sum;
}

template <class T>
void SYCLStream<T>::init_arrays(T initA, T initB, T initC)
{
  queue->submit([&](handler &cgh)
  {
    auto ka = d_a->template get_access<access::mode::write>(cgh);
    auto kb = d_b->template get_access<access::mode::write>(cgh);
    auto kc = d_c->template get_access<access::mode::write>(cgh);
    cgh.parallel_for<init_kernel>(p->get_kernel<init_kernel>(),
      range<1>{array_size}, [=](item<1> item)
    {
      auto id = item.get()[0];
      ka[id] = initA;
      kb[id] = initB;
      kc[id] = initC;
    });
  });
  queue->wait();
}

template <class T>
void SYCLStream<T>::read_arrays(std::vector<T>& a, std::vector<T>& b, std::vector<T>& c)
{
  auto _a = d_a->template get_access<access::mode::read, access::target::host_buffer>();
  auto _b = d_b->template get_access<access::mode::read, access::target::host_buffer>();
  auto _c = d_c->template get_access<access::mode::read, access::target::host_buffer>();
  for (int i = 0; i < array_size; i++)
  {
    a[i] = _a[i];
    b[i] = _b[i];
    c[i] = _c[i];
  }
}

void getDeviceList(void)
{
  std::vector<platform> platforms = platform::get_platforms();

  for (unsigned i = 0; i < platforms.size(); i++)
  {
    std::vector<device> plat_devices = platforms[i].get_devices();
    devices.insert(devices.end(), plat_devices.begin(), plat_devices.end());
  }
  cached = true;
}

void listDevices(void)
{
  getDeviceList();

  if (devices.size() == 0)
  {
    std::cerr << "No devices found." << std::endl;
  }
  else
  {
    std::cout << std::endl;
    std::cout << "Devices:" << std::endl;
    for (int i = 0; i < devices.size(); i++)
    {
      std::cout << i << ": " << getDeviceName(i) << std::endl;
    }
    std::cout << std::endl;
  }
}

std::string getDeviceName(const int device)
{
  if (!cached)
    getDeviceList();

  std::string name;

  if (device < devices.size())
  {
    name = devices[device].get_info<info::device::name>();
  }
  else
  {
    throw std::runtime_error("Error asking for name for non-existant device");
  }

  return name;
}

std::string getDeviceDriver(const int device)
{
  if (!cached)
    getDeviceList();

  std::string driver;

  if (device < devices.size())
  {
    driver = devices[device].get_info<info::device::driver_version>();
  }
  else
  {
    throw std::runtime_error("Error asking for driver for non-existant device");
  }

  return driver;
}


template class SYCLStream<float>;
template class SYCLStream<double>;
