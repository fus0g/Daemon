// Process entry point. All the work happens in the daemon.app module (see cli.cpp).
extern "C" int daemon_main(int argc, char** argv);

int main(int argc, char** argv) { return daemon_main(argc, argv); }
