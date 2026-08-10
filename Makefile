# Прогон тестов переносимой логики без железа и PlatformIO.
CXX ?= g++
FLAGS = -std=c++17 -Wall -Wextra -Iinclude

# Проверка сборки прошивки БЕЗ платы и без PlatformIO.
#
# Настоящих библиотек платы здесь нет, но ошибки, которые я допускаю чаще всего —
# необъявленная переменная, лишний аргумент, sizeof от указателя, — ловятся и по
# заглушкам из test/stubs. Раньше о них сообщал лог сборки у человека через полчаса
# после отправки; теперь они не выходят за пределы этой машины.
check:
	@fail=0; \
	for f in src/*.cpp; do \
	  out=$$($(CXX) -std=gnu++17 -fsyntax-only -Iinclude -Itest/stubs $$f 2>&1); \
	  if [ -n "$$out" ]; then echo "=== $$f"; echo "$$out" | head -20; fail=1; fi; \
	done; \
	if [ $$fail -eq 0 ]; then echo "прошивка: синтаксис сошёлся ($$(ls src/*.cpp | wc -l) файлов)"; \
	else echo "ЕСТЬ ОШИБКИ СБОРКИ"; exit 1; fi

all: check test

test:
	@$(CXX) $(FLAGS) src/voile_frame.cpp src/voile_diversity.cpp test/test_core.cpp -o /tmp/vd_core
	@/tmp/vd_core
	@echo
	@$(CXX) $(FLAGS) src/voile_ratchet.cpp test/crypto_test_backend.cpp test/test_ratchet.cpp -o /tmp/vd_ratchet
	@/tmp/vd_ratchet
	@echo
	@$(CXX) $(FLAGS) src/audio.cpp src/transport.cpp src/store.cpp src/media.cpp test/test_media.cpp -o /tmp/vd_media
	@/tmp/vd_media
	@echo
	@$(CXX) $(FLAGS) src/input_logic.cpp test/test_input.cpp -o /tmp/vd_input
	@/tmp/vd_input
	@echo
	@$(CXX) $(FLAGS) test/test_phone.cpp -o /tmp/vd_phone
	@/tmp/vd_phone
	@echo
	@$(CXX) $(FLAGS) src/contacts.cpp test/crypto_test_backend.cpp test/test_contacts.cpp -o /tmp/vd_contacts
	@/tmp/vd_contacts
	@echo
	@$(CXX) $(FLAGS) src/contacts.cpp test/crypto_test_backend.cpp test/test_pairing.cpp -o /tmp/vd_pairing
	@/tmp/vd_pairing

.PHONY: test
