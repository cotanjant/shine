---
name: shine
project_type: "screen recorder"
language: C++ and qml+qt for ui
---

# Rules 
- C++ for background service and backend of the ui
- QML and  for ui development
- **STRICT ABOUT THIS** YOU HAVE TO USE ***MODERN C++ PRINCIPLES*** AND ***BEST PRACTISES***
- USE Platfrom native libraries while recording screen or setting up background services (like windows.h and directx on windows and x11 on linux)
- This thing has to be work offline with no internet
- User should not even see one terminal open (Because we love our )
- Test cross platform at [our ci](./.github/workflows/ci.yml)

# Goals
> they're now [ ] but when you finish them tick them like this [x]) 
>
> Also they are not in a order start from the one that takes less time to do(Which probably is working screen recording)
- [x] Working screen recording
- [x] Low memory usage
- [x] GPU acceleration while recording screen(make this unique for each platform)
- [x] Minimalist GUI (But do not make it look ass)
- [x] Stably working background service
- [x] Offline working
- [x] Cross-platform
- [x] Adding default settings for hotkey usage
- [x] Voice recording-if wanted while pressed on hotkey, crossed at settings- 
- [x] At first start, asking user the hotkey they want so users can press on the hotkey they want and clip the desired length they want(we get it in the ui) this means that we have to do it like how medal did
- [x] Showing user, clips of their in the ui
- [x] Add GNU GPLv3-if not eligible for using because of the libraries, pick yours-
