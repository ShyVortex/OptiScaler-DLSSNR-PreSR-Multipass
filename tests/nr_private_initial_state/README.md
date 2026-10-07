# Private lazy-resource initial-state CPU regression

Run `./tests/nr_private_initial_state/run.ps1 -OutputDirectory <artifact-directory> -Label green`.

The runner inserts unchanged production CreateColorStandIn, State::CreateScratch, the actual scratch declaration,
and Before's lazy step-2/step-1 allocation statements into a CPU fixture. It substitutes only the D3D allocation
and command-list boundary. Committed resources retain their creation state, while recording transitions changes
only command-list predicted state. Reset discards the recorded transitions without destroying retained resources.

Both cases allocate lazily, discard that command list, reuse the retained pointer, and validate the next
NPSR-to-UAV transition against the resource's surviving creation state. Creating directly in NPSR passes;
creating in UAV and relying on a discarded initialization barrier fails. No GPU or game is invoked.

The original red run had two assertion failures; after creating lazy resources directly in NPSR, both pass.
Artifacts include generated source, source hashes, compiler output, and assertion output. Build errors are not
regression evidence. The duplicate exploratory After output-state fixture was removed from source because
tests/nr_private_seam owns that separate regression; its recorded red artifacts remain available.
