# docs/archive — исторический материал (не применять)

Здесь лежат документы, которые больше не являются актуальными источниками требований или статусов.
**Читать как архив.** Единственный эталон ТЗ — [../OMNIA_MASTER_SPEC_v2.0.md](../OMNIA_MASTER_SPEC_v2.0.md);
единый сводный статус реализации — [../REQUIREMENTS_STATUS_RU.md](../REQUIREMENTS_STATUS_RU.md).

| Файл | Назначение в прошлом | Чем заменён |
|---|---|---|
| `REQUIREMENTS_AUDIT_RU.md` | Журнал аудита расхождений (AUD-001…) | REQUIREMENTS_STATUS_RU.md |
| `FULL_REQUIREMENTS_REVIEW_RU.md` | Сверка источников и функциональных разделов | REQUIREMENTS_STATUS_RU.md |
| `REQUIREMENTS_MATRIX_PART1_RU.md` | Покрытие строк 1–47 оригинала | REQUIREMENTS_STATUS_RU.md (постатейно, без обезличенного построчного копирования) |
| `REQUIREMENTS_MATRIX_PART2_RU.md` | Покрытие строк 48–125 оригинала (АЦП-кнопки и т.д.) | REQUIREMENTS_STATUS_RU.md |
| `TFT_UNVERIFIED_RU.md`, `ST7789_8080_8P_MOD_GUIDE_UNVERIFIED.png` | Непроверенные инструкции перепайки TFT | текущее решение по TFT — по фотографиям вашей платы и [SCHEMES_INDEX_RU.md](../SCHEMES_INDEX_RU.md); ничего не перепаивать до подтверждения |
| `OMNIA_SCHEMATIC_IC_TRANSISTORS_OLD.png`, `OMNIA_WIRING_STAGE1_CORE_OLD.png` | Устаревшие рисунки схем/монтажа | [SCHEMES_INDEX_RU.md](../SCHEMES_INDEX_RU.md) |

Архивные документы сохраняются ради истории решений (почему, например, 15 User PEQ → 10 + 5 Room;
почему 192 кГц не обещано). Они не являются монтажными инструкциями и не отменяют ТЗ.
При любом расхождении действует ТЗ; при расхождении с измерениями — результаты измерений.## Известные дефекты исторических снимков (не исправлять — они закреплены sha256 в tests/test_project_contract.py)

- `docs/reference/OMNIA_MASTER_GITHUB_WIP_ffc6859.md`: относительная ссылка `SJR_BTM334_QCC3034_GUIDE.md` битая (файл лежит в `docs/`, снимок — в `docs/reference/`); корректный путь — `../SJR_BTM334_QCC3034_GUIDE.md`. Байты снимка неизменны намеренно.


