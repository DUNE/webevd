# webevd

Web event display for LArSoft data products

## Usage

1. Setup the products:
```
setup webevd <version> -q <qualifier>
setup dunesw <same version> -q <same qualifier>
```
2. Look in `webevd/WebEVD/fcl` for the fcl with the appropriate geometry, if it's not there make one your own!
3. Run the fcl on your file of interest and follow the instructions it spits out to view the EVD.

**Note:** The ups release of this package can sometimes lag behind. In these cases, you may need to build locally with the `lardata` and/or `gallery` versions in `ups/product_deps` updated. You may also need to add an new build qualifier row, this is unlikely though.

## Copyright and Licensing
Copyright © 2023 University College London for the benefit of the DUNE Collaboration.

This repository, and all software contained within, except where noted within the individual source files, is licensed under
the Apache License, Version 2.0 (the "License"); you may not use this
file except in compliance with the License. You may obtain a copy of
the License at

    http://www.apache.org/licenses/LICENSE-2.0

Copyright is granted to UNIVERSITY COLLEGE LONDON on behalf
of the Deep Underground Neutrino Experiment (DUNE). Unless required by
applicable law or agreed to in writing, software distributed under the
License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
CONDITIONS OF ANY KIND, either express or implied. See the License for
the specific language governing permissions and limitations under the
License.
