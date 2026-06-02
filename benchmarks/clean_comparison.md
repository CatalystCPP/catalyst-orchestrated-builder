| Command | Mean [ms] | Min [ms] | Max [ms] | Relative |
|:---|---:|---:|---:|---:|
| `cob -C heavy_repo -t clean` | 15.9 ± 1.7 | 14.5 | 20.4 | 1.37 ± 0.30 |
| `ninja -C heavy_repo -t clean` | 11.7 ± 2.2 | 9.8 | 16.0 | 1.00 |
| `make -C heavy_repo clean` | 351.9 ± 14.0 | 334.9 | 373.9 | 30.21 ± 5.86 |
