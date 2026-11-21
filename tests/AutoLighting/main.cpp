#include <iostream>
#include "../../iot/espnode/AutoLighting.h"
#define CHECK(test) do { if (!(test)) { std::cerr << "FAIL line " << __LINE__ << ": " #test << '\n'; return 1; } } while (0)
int main() {
  using namespace AutoLighting;
  CHECK(RequiredSamples == 5);
  CHECK(useSensor(true, true, 22*60, 360, 1080));
  CHECK(useSensor(true, false, 0, 360, 1080));
  CHECK(!useSensor(false, false, 10*60, 360, 1080));
  CHECK(useSensor(false, true, 360, 360, 1080));
  CHECK(!useSensor(false, true, 1080, 360, 1080));
  CHECK(!useSensor(false, true, 359, 360, 1080));
  CHECK(useSensor(false, true, 23*60, 20*60, 6*60));
  ConsecutiveSamples sensor;
  bool on = true;
  for (int i=0;i<4;++i) { on = sensor.update(true, 60, on); CHECK(on); }
  on = sensor.update(true, 60, on); CHECK(!on);
  for (int i=0;i<4;++i) { on = sensor.update(true, 10, on); CHECK(!on); }
  on = sensor.update(true, 10, on); CHECK(on);
  sensor.update(true, 60, on);
  sensor.update(false, 60, on); CHECK(sensor.pendingSamples() == 0);
  sensor.update(true, 60, on);
  sensor.update(true, 50, on); CHECK(sensor.pendingSamples() == 0);
  sensor.update(true, 60, on);
  sensor.update(true, 10, on); CHECK(sensor.pendingSamples() == 0);
  sensor.update(true, 60, on);
  sensor.reset(); CHECK(sensor.pendingSamples() == 0);
  std::cout << "PASS full_time/scheduled, boundaries, RTC-independent full_time and 5 consecutive samples\n";
}
