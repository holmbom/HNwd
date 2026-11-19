# Moduler, ett CMake-mål per katalog. Beroenden pekar åt ett håll:
#
#   core      filformatet: container, uppackning, paketens avkodare    noll beroenden
#   scene     det publika API:t: Document, länkkontroller, trädet       beror på core
#
# Gränsen upprätthålls av include-stigarna i varje moduls CMakeLists, så ett
# felaktigt beroende blir ett kompileringsfel.
add_subdirectory(core)
add_subdirectory(scene)
