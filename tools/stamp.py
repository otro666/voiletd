# -*- coding: utf-8 -*-
# Штамп сборки: дата и коммит зашиваются определением VUAL_BUILD_STAMP.
#
# Зачем: спор «прошился ли новый файл» решается одной строкой в порту и на экране
# «Состояние», а не пересборками наугад. Коммит берётся из среды сборки (его туда
# кладёт GitHub), локально остаётся «local».
import time, os
Import("env")

sha = (os.environ.get("GITHUB_SHA") or "local")[:7]
stamp = time.strftime("%Y-%m-%d %H:%M") + " " + sha
env.Append(CPPDEFINES=[("VUAL_BUILD_STAMP", env.StringifyMacro(stamp))])
print("штамп сборки:", stamp)
