# Прогон тестов переносимой логики без железа и PlatformIO.
CXX ?= g++
FLAGS = -std=c++17 -Wall -Wextra -Iinclude

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
