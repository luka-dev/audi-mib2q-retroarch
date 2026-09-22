# MU1316: граница для собственного Adreno renderer
Дата: 2026-09-08. Статический разбор оригинальной извлечённой прошивки с r2/rabin2.

## Вывод
В этой прошивке есть доступная из пользовательского процесса граница ниже GLES:
`OpenGLES20.so → libGSLUser.so → MsgSendv /dev/kgsl-3D → kgsl + GSLKernel-A320.so → GPU`.

Клиент передаёт готовые IB как пары GPU-адрес/длина в словах. Это конкретная
точка для собственного генератора команд и потенциального QNX backend для
Freedreno. Работу команд, сгенерированных самостоятельно, на настоящем GPU
в этом исследовании ещё не проверяли. Полный перенос Mesa не выполнен.

## Бинарники
Источник:
`/Users/luka/Desktop/AUDI_2/Firmwares/HU/MHI2Q_US_AUG22_P5087_MU1316/extracted/ifs2/ifs_display/proc/boot`.

ELF32 ARM little-endian EABI5. Библиотеки PIC, база 0; kgsl executable
имеет базу 0x100000, интерпретатор /usr/lib/ldqnx.so.2.
Автоопределение r2 «os linux» неверно для этих QNX ELF.
У libGSLUser LOAD0 VA = file offset, поэтому приведённые адреса его кода
одновременно являются смещениями в файле.

| Файл | SHA-256 |
|---|---|
| OpenGLES20.so | cc89187b21c921f109e7802ac805879a52002883c0d001355aafc44fd93bbc4d |
| libGSLUser.so | 3e8d255432e67de7ae517ec5a79921c978c05d1c9d61ffcb0e9a882f8f080b90 |
| GSLKernel-A320.so | 0841078330da901edcfaae7f0cb53fbce0c06fbbf2dd5f9fed80dd6c88a06912 |
| kgsl | a4408fccf471b326cfb21322dcbcb3bbabf09c8969ae9a036881eec934e278f7 |
| libOSUser.so | ae3950cd13f7d47e816fa98285378a51d7f032257d07340a6f898e12a09d4045 |

Поиск файлов с этими именами под Firmwares нашёл только этот извлечённый
комплект. Это не исключает доноров внутри нераспакованных образов.

## Проверенные границы и адреса
OpenGLES20 DT_NEEDED включает libGSLUser и libOSUser; импортирует
gsl_command_issueib_sync (PLT 0x146a0), gsl_memory_alloc_pure (0x145d4),
gsl_context_create (0x145c8), gsl_command_waittimestamp (0x14af0),
gsl_memory_cacheoperation (0x149b8).

libGSLUser экспортирует:
- gsl_library_open 0x37a4; open() вызывается по 0x3954; строка /dev/kgsl-3D 0x77f1.
- gsl_context_create 0x1b4c; gsl_context_destroy 0x25c8.
- gsl_memory_alloc_pure 0x2fb4; gsl_memory_free_pure 0x3388.
- gsl_command_issueib 0x2558; gsl_command_issueib_sync 0x2378;
  gsl_command_issueib_with_alloc_list 0x2260.
- gsl_command_readtimestamp 0x2108; gsl_command_waittimestamp 0x2094.
- gsl_memory_cacheoperation 0x4350.

GSLKernel-A320 — аппаратная часть в QNX resource-manager процессе.
DT_NEEDED: libOSKernel, libpmem_client, libpmemext, libmmpm2_client_af,
libsmmu_client, libdalsys, libmmap_peer, libc.
Код вызывает resmgr_attach (например 0x250b4), mmap_peer (0x212fc,
0x2263c, 0x32658), smmu_map (0x2fc04). Это свидетельство того, что управление
памятью/устройством можно оставить штатному нижнему слою.
Строка kgsl_cmdstream_3d_issueibcmds находится по 0x4ea14; диагностическая
сигнатура с devhandle/context_id/ib/numibs/timestamp/flags — по 0x4f3bc.
Адреса kernel здесь VA; не использовать как непроверенные точки патча.

## Протокол MsgSendv
Помощники libGSLUser 0x45f0 и 0x6164 собирают 8-байтный QNX заголовок:
u16 type=0x113, combine_len=8, mgrid=0xf000, subtype.
Подтверждение: 0x4714–0x4758 и 0x6240–0x628c.
Это _IO_MSG, не Linux KGSL ioctl и не QNX devctl.

| subtype | Назначение | Подтверждение в libGSLUser |
|---|---|---|
| 0x900 | Вход/регистрация библиотеки/клиента | 0x39ac, payload 8 B, reply 20 B |
| 0x910 | Выход/закрытие библиотеки/клиента | runtime: stock `gsl_library_close` в QEMU |
| 0x920 | Открытие device | 0x3f94–0x3fec, payload 8 B |
| 0x921 | Закрытие device | 0x2dc4–0x2dfc, payload 4 B |
| 0x923 | Получение свойства device | helper 0x3ba4, payload 12 B |
| 0x950 | Создание контекста | 0x6e38, payload 12 B, reply 4 B |
| 0x951 | Уничтожение контекста | 0x6d9c, payload 8 B |
| 0x960 | GPU allocation | 0x30e0 и 0x327c, payload 16 B, reply 24 B |
| 0x961 | Освобождение памяти | 0x34d0 + descriptor |
| 0x930 | Отправка списка IB | 0x6398, payload 20 B + 8 B на IB, reply 4 B |
| 0x931 | Чтение timestamp | 0x7174, payload 12 B, reply 4 B |
| 0x933 | Ожидание timestamp | 0x7240, payload 16 B |
| 0x991 | Cache operation | 0x43d8, payload 16 B + descriptor 24 B |

Код 0x6310–0x63d0 сериализует submission:
msg+8 device; +12 context; +16 numibs; +20 flags; +24 входное значение
timestamp; с +28 пары {gpuaddr, sizedwords}. В 0x6364–0x6380 адрес получается
из memdesc+8 плюс offset из IB; длина берётся из IB+8.
Это внутренний 32-байтный IB, а не публичный аргумент OpenGLES. Обёртка
gsl_command_issueib_sync 0x2378 принимает массив 16-байтных direct-IB:
первые 8 B копирует в memdesc+8, sizedwords читает по +8 и шагает по массиву
на 0x10 B (0x248c–0x24c0). Затем она создаёт внутренний 32-байтный элемент,
где memdesc pointer находится по +0, sizedwords по +8, offsetbytes по +16.
OpenGLES20 вызывает эту функцию по 0x9d0a4/0x9d194/0x9d23c/0x9d2f4;
перед каждым вызовом r0/r1/r2/r3 и три stack-аргумента подтверждают семь
аргументов: device, context, direct_ib[], numibs, timestamp*, flags, syncobj.
Смысл отдельных flags требует проверки серверной стороны.

## Важное отличие структур памяти
Публичный memdesc занимает 32 B, wire memdesc — 24 B.
Код alloc/cache/free копирует публичные поля +0,+8,+16,+20,+24,+28
в шесть последовательных wire dwords. Это видно в 0x2ffc–0x3028,
0x3228–0x325c, 0x439c–0x43c4.
Поле +8 публичной структуры используется как GPU address в submit;
+16 — размер. Поле +12 при возврате alloc обнуляется. Layout согласуется
одновременно с выравниванием AAPCS и со всеми путями alloc/cache/free:

```c
struct gsl_memdesc_arm32 {
    uint32_t hostptr;      /* +0 */
    uint32_t reserved04;   /* +4: не передаётся */
    uint64_t gpuaddr;      /* +8; high dword очищается */
    uint64_t size;         /* +16 */
    uint64_t flags;        /* +24 */
};                         /* sizeof = 32 */

struct gsl_memdesc_wire {
    uint32_t hostptr;      /* +0 */
    uint32_t gpuaddr;      /* +4 */
    uint64_t size;         /* +8 */
    uint64_t flags;        /* +16 */
};                         /* sizeof = 24 */
```

Не объявлять публичный descriptor как шесть последовательных uint32_t и не
делать его memcpy на wire: библиотека намеренно пропускает public +4/+12.
Правила владения и полная семантика flags ещё не восстановлены.

Проверяемое C-описание с compile-time asserts находится в
`tools/qnx-gsl-port/qnx_gsl_abi.h`.

## Восстановленные public signatures
Для первого стендалон-проба достаточно следующего подтверждённого подмножества:

```c
int32_t  gsl_library_open(uint32_t flags);
uint32_t gsl_device_open(uint32_t device_id, uint32_t flags); /* id == 1 */
int32_t  gsl_device_close(uint32_t device);
uint32_t gsl_context_create(uint32_t device, uint32_t type, uint32_t flags);
int32_t  gsl_context_destroy(uint32_t device, uint32_t context);
int32_t  gsl_memory_alloc_pure(uint32_t size, uint32_t flags,
                               struct gsl_memdesc_arm32 *out);
int32_t  gsl_memory_free_pure(struct gsl_memdesc_arm32 *memdesc);
int32_t  gsl_memory_cacheoperation(struct gsl_memdesc_arm32 *memdesc,
                                   uint32_t offset, uint32_t size,
                                   uint32_t operation);
int32_t  gsl_command_issueib_sync(uint32_t device, uint32_t context,
                                  const struct gsl_direct_ib_arm32 *ibs,
                                  uint32_t numibs, uint32_t *timestamp,
                                  uint32_t flags, const void *syncobj);
int32_t  gsl_command_readtimestamp(uint32_t device, uint32_t context,
                                   uint32_t type, uint32_t *timestamp);
int32_t  gsl_command_waittimestamp(uint32_t device, uint32_t context,
                                   uint32_t timestamp, uint32_t timeout);
```

`gsl_context_destroy` сохраняет двухаргументный ABI, хотя реализация ищет
контекст по второму аргументу. Прототип простого `gsl_command_issueib` пока
не включён в заголовок: GLES использует точнее восстановленный `_sync` путь.

## Синхронизация
0x7010 читает timestamp через быстрый путь общей памяти при определённых
флагах; иначе формирует 0x931. Payload {device, context, type}.
Различает type 1/2/3; локальные ветви для 1 и 2 используют разные offsets.
0x71b8 проверяет завершение через type=2.
0x71f4 сначала проверяет timestamp, затем использует 0x933; при определённой
ошибке переходит на повторную проверку с gfx_os_sleep.
Нельзя считать подтверждение приёма IB завершением GPU или освобождать
используемую память сразу после MsgSendv.

## Повторное использование исследования QEMU
В ../mhi2q-qemu/guest/kgsl_resmgr/kgsl_resmgr.c уже реализована обратная
сторона этого протокола для эмуляции. Документация docs/04-gpu-paravirt.md
дала полезные ориентиры; клиентская сериализация проверена заново по ELF.
Эмулятор содержит синтетические handles/адреса, заглушки ответов и
собственную очередь. Их нельзя переносить в клиент настоящего GPU.
Старый комментарий с адресом kernel 0x2ff94 не подтверждён: на текущем
бинарнике этот адрес внутри другой последовательности, а не надёжно
установленное начало attach-функции.

## Практический путь порта
1. Начать с libGSLUser как транспорта и восстановленных signatures; прямой
   MsgSendv backend — второй вариант. Сначала read-only open/getinfo без IB.
2. Отдельный offscreen тест: allocate → CPU write/cache clean → собственный
   минимальный IB → retired timestamp → cache invalidate → проверка результата.
   Сначала проверить простую запись/копирование, затем треугольник.
3. Согласовать ownership контекста: какие регистры и GMEM-shadow сохраняет
   штатный kernel, что renderer должен восстанавливать сам.
4. Адаптировать Freedreno BO/submit/fence и QNX системные зависимости.
5. После проверки GPU — импорт/экспорт буферов Screen/EGL, stride/tiling
   и синхронизация вывода; до этого достаточно offscreen readback.
6. Сравнить тот же GLES workload по времени и пикселям. Снижение overhead
   нового renderer пока гипотеза, а не измеренный результат.

Прошивка не изменена; команды GPU в этом исследовании не отправлялись.
