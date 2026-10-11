# CS370-Term-Project
## Workflow

1. Branch off `main`: `git switch -c my-feature`
2. Push and open a pull request.
3. Once approved and merged into `main`, GitHub Actions deploys automatically to the Pi
   (`.github/workflows/deploy.yml` → files synced to `/srv/cs370`, then `deploy.sh` runs).

Watch deploys in the **Actions** tab. Put any build/restart steps in `deploy.sh`.
